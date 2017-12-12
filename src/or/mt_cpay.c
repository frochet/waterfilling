/**
 * \file mt_cpay.c
 *
 * Implement logic for the client role in the moneTor payment scheme. The module
 * interacts with other payment code (<b>mt_cpay<\b>, <b>mt_rpay<\b>,
 * <b>mt_ipay<\b>) across the Tor network. The module only interacts with two
 * other parts of the Tor code base: the corresponding moneTor controller and
 * the cpuworker. Interactions with controllers are managed through descriptors
 * defined by the struct <b>mt_desc_t<\b>. These descriptors serve as unique
 * payment identifies for the payment module such that the controller can
 * abstract away all network connection details.
 *
 * The following interface is made available to the controller:
 *   <ul>
 *     <li>mt_cpay_init();
 *     <li>mt_cpay_pay()
 *     <li>mt_cpay_close()
 *     <li>mt_cpay_recv()
 *   <\ul>
 *
 * Conversely, the module requires access to the following controller interface:
 *   <ul>
 *     <li>mt_send_message()
 *     <li>mt_send_message_multidesc()
 *     <li>mt_alert_payment()
 *   <\ul>
 *
 * The payment module manages a collection of payment channels each of which is
 * roughly implemented as a state machine. Channels only have a well-defined
 * state inbetween protocol executions; inbetween then are in a limbo
 * "transition" state. These active protocols are tracked by protocol ids (pid)s
 * that are probabilistically assumed to be globally unique
 *
 * The code features a "re-entrancy" pattern whereby the same function is called
 * again and again via callbacks until the channel is in the right state to
 * complete the task.
 */

#pragma GCC diagnostic ignored "-Wswitch-enum"
#pragma GCC diagnostic ignored "-Wunused-function"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "or.h"
#include "workqueue.h"
#include "cpuworker.h"
#include "mt_common.h"
#include "mt_cpay.h"

#define NON_NULL 1

/**
 * Prototype for multi-thread function used to generate the expensive zkp proof
 */
typedef void (*work_task)(void*);

/**
 * Hold function and arguments necessary to execute callbacks on a channel once
 * the current protocol has completed
 */
typedef struct {
  // callback function
  int (*fn)(mt_desc_t*, mt_desc_t*);

  // args
  mt_desc_t dref1;
  mt_desc_t dref2;
} mt_callback_t;

/**
 * Hold information necessary to maintain a single payment channel
 */
typedef struct {
  mt_desc_t rdesc;
  mt_desc_t idesc;
  chn_end_data_t data;

  mt_callback_t callback;
} mt_channel_t;

/**
 * Hold arguments need to run the multi-thread workqueue for the expensive zkp
 * proof generation
 */
typedef struct {
  mt_channel_t* chn;
  byte pid[DIGEST_LEN];
} mt_wcom_args_t;

/**
 * Single instance of a client payment object
 */
typedef struct {

  byte pp[MT_SZ_PP];
  byte pk[MT_SZ_PK];
  byte sk[MT_SZ_SK];
  byte addr[MT_SZ_ADDR];
  int mac_balance;
  int chn_balance;
  int chn_number;

  mt_desc_t ledger;
  int fee;
  int tax;

  // channel states are encoded by which of these containers they are held
  smartlist_t* chns_setup;
  smartlist_t* chns_estab;
  smartlist_t* nans_setup;
  digestmap_t* nans_estab;        // digest(rdesc) -> channel
  digestmap_t* nans_destab;       // digest(rdesc) -> channel
  digestmap_t* nans_reqclosed;    // digest(rdesc) -> channel
  smartlist_t* chns_spent;

  // special container to hold channels in the middle of a protocol
  digestmap_t* chns_transition;   // pid -> channel
} mt_cpay_t;

// functions to initialize new protocols
static int init_chn_end_setup(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_chn_end_estab1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_nan_cli_setup1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_nan_cli_estab1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_nan_cli_pay1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_nan_cli_destab1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_nan_cli_dpay1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_nan_cli_reqclose1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);
static int init_nan_end_close1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]);

// functions to handle incoming recv messages
static int handle_any_led_confirm(mt_desc_t* desc, any_led_confirm_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_chn_int_estab2(mt_desc_t* desc, chn_int_estab2_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_chn_int_estab4(mt_desc_t* desc, chn_int_estab4_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_setup2(mt_desc_t* desc, nan_int_setup2_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_setup4(mt_desc_t* desc, nan_int_setup4_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_setup6(mt_desc_t* desc, nan_int_setup6_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_rel_estab6(mt_desc_t* desc, nan_rel_estab6_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_rel_pay2(mt_desc_t* desc, nan_rel_pay2_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_destab2(mt_desc_t* desc, nan_int_destab2_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_dpay2(mt_desc_t* desc, nan_int_dpay2_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_rel_reqclose2(mt_desc_t* desc, nan_rel_reqclose2_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_close2(mt_desc_t* desc, nan_int_close2_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_close4(mt_desc_t* desc, nan_int_close4_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_close6(mt_desc_t* desc, nan_int_close6_t* token, byte (*pid)[DIGEST_LEN]);
static int handle_nan_int_close8(mt_desc_t* desc, nan_int_close8_t* token, byte (*pid)[DIGEST_LEN]);

// special helper functions for protocol steps involving a zkp proof generation
static int help_chn_end_estab1(void* args);
static int help_chn_int_estab4(void* args);
static int help_nan_int_close8(void* args);

// miscallaneous helper functions
static int pay_helper(mt_desc_t* rdesc, mt_desc_t* idesc);
static int dpay_helper(mt_desc_t* rdesc, mt_desc_t* idesc);

static mt_channel_t* new_channel(void);
static int compare_chn_end_data(const void** a, const void** b);
static mt_channel_t* smartlist_idesc_remove(smartlist_t* list, mt_desc_t* desc);
static workqueue_reply_t wcom_task(void* thread, void* arg);
static void wallet_reply(void* arg);
static int pay_finish(mt_desc_t* rdesc, mt_desc_t* idesc);
static int close_finish(mt_desc_t* rdesc, mt_desc_t* idesc);

static mt_cpay_t client;

/**
 * Initialize the module; should only be called once. All necessary variables
 * will be loaded from the torrc configuration file.
 */
int mt_cpay_init(void){

  // TODO: get this to work
  // cpu_init();

  byte pp[MT_SZ_PP];
  byte pk[MT_SZ_PK];
  byte sk[MT_SZ_SK];
  mt_desc_t ledger;
  int fee;
  int tax;
  int cli_bal;

  /********************************************************************/
  //TODO replace with torrc

  FILE* fp;

  fp = fopen("mt_config_temp/pp", "rb");
  tor_assert(fread(pp, 1, MT_SZ_PP, fp) == MT_SZ_PP);
  fclose(fp);

  fp = fopen("mt_config_temp/cli_pk", "rb");
  tor_assert(fread(pk, 1, MT_SZ_PK, fp) == MT_SZ_PK);
  fclose(fp);

  fp = fopen("mt_config_temp/cli_sk", "rb");
  tor_assert(fread(sk, 1, MT_SZ_SK, fp) == MT_SZ_SK);
  fclose(fp);

  fp = fopen("mt_config_temp/led_desc", "rb");
  tor_assert(fread(&ledger, 1, sizeof(mt_desc_t), fp) == sizeof(mt_desc_t));
  fclose(fp);

  fp = fopen("mt_config_temp/fee", "rb");
  tor_assert(fread(&fee, 1, sizeof(fee), fp) == sizeof(fee));
  fclose(fp);

  fp = fopen("mt_config_temp/tax", "rb");
  tor_assert(fread(&tax, 1, sizeof(tax), fp) == sizeof(tax));
  fclose(fp);

  fp = fopen("mt_config_temp/cli_bal", "rb");
  tor_assert(fread(&cli_bal, 1, sizeof(cli_bal), fp) == sizeof(cli_bal));
  fclose(fp);

  /********************************************************************/

  // copy in values crypto fields
  memcpy(client.pp, pp, MT_SZ_PP);
  memcpy(client.pk, pk, MT_SZ_PK);
  memcpy(client.sk, sk, MT_SZ_SK);
  mt_pk2addr(&client.pk, &client.addr);
  client.ledger = ledger;
  client.fee = fee;
  client.tax = tax;
  client.mac_balance = cli_bal;
  client.chn_balance = 0;
  client.chn_number = 0;

  // initialize channel containers
  client.chns_setup = smartlist_new();
  client.chns_estab = smartlist_new();
  client.nans_setup = smartlist_new();
  client.nans_estab = digestmap_new();
  client.nans_destab = digestmap_new();
  client.nans_reqclosed = digestmap_new();
  client.chns_spent = smartlist_new();
  client.chns_transition = digestmap_new();

  // TODO generate new channels
  return MT_SUCCESS;
}

/**
 * Send a single payment to the relay through a given intermediary. If
 * <b>rdesc<\b> and <b>idesc<\b> are equal, then the payment module will make a
 * direct payment to the intermediary module. If a payment request to a given
 * relay is made with a different intermediary BEFORE the previous
 * relay/intermediary payment pair was closed, then this function will return an
 * error.
 */
int mt_cpay_pay(mt_desc_t* rdesc, mt_desc_t* idesc){

  // determine whether this is a standard or direct payment
  if(rdesc->id != idesc->id)
    return pay_helper(rdesc, idesc);
  else
    return dpay_helper(rdesc, idesc);
}

/**
 * Handle standard payments from mt_cpay_pay(). Re-enter this payment again and
 * again until the payment is successful and pay_finish is called.
 */
static int pay_helper(mt_desc_t* rdesc, mt_desc_t* idesc){
  mt_channel_t* chn;
  byte digest[DIGEST_LEN];
  mt_desc2digest(rdesc, &digest);
  byte pid[DIGEST_LEN];
  mt_crypt_rand(DIGEST_LEN, pid);

  // close the standard nanopayment channel if possible; callback close_finish
  if((chn = digestmap_remove(client.nans_reqclosed, (char*)digest))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = pay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_end_close1(chn, &pid);
  }

  // if maximum payments reached then close the current channel
  if((chn = digestmap_get(client.nans_estab, (char*)digest)) &&
     chn->data.nan_state.num_payments == chn->data.nan_public.num_payments){
    digestmap_remove(client.nans_estab, (char*)digest);
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = pay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_reqclose1(chn, &pid);
  }

  // make payment if possible; callback pay_finish
  if((chn = digestmap_remove(client.nans_estab, (char*)digest))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = pay_finish, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_pay1(chn, &pid);
  }

  // establish nanopayment channel if possible; callback pay_helper
  if((chn = smartlist_pop_last(client.nans_setup))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->rdesc = *rdesc;
    chn->callback = (mt_callback_t){.fn = pay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_estab1(chn, &pid);
  }

  // set up nanopayment channel if possible; callback pay_helper
  if((chn = smartlist_pop_last(client.chns_estab))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = pay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_setup1(chn, &pid);
  }

  // establish channel if possible; callback pay_helper
  if((chn = smartlist_pop_last(client.chns_setup))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = pay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_chn_end_estab1(chn, &pid);
  }

  // set up channel if possible; callback pay_helper
  if(client.mac_balance >= MT_CLI_CHN_VAL + client.fee){
    chn = new_channel();
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->idesc = *idesc;    // set channel intermediary
    chn->callback = (mt_callback_t){.fn = pay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_chn_end_setup(chn, &pid);
  }

  log_debug(LD_MT, "insufficient funds to create new channel");
  return MT_ERROR;
}

/**
 * Handle direct payments from mt_cpay_pay(). Re-enter this payment again and
 * again until the payment is successful and pay_finish is called.
 */
static int dpay_helper(mt_desc_t* rdesc, mt_desc_t* idesc){
  mt_channel_t* chn;
  byte digest[DIGEST_LEN];
  mt_desc2digest(rdesc, &digest);
  byte pid[DIGEST_LEN];
  mt_crypt_rand(DIGEST_LEN, pid);

  // if maximum payments reached then close the current channel
  if((chn = digestmap_get(client.nans_destab, (char*)digest)) &&
     chn->data.nan_state.num_payments == chn->data.nan_public.num_payments){
    digestmap_remove(client.nans_destab, (char*)digest);
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = dpay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_end_close1(chn, &pid);
  }

  // if maximum payments reached then close the current channel
  if((chn = digestmap_remove(client.nans_estab, (char*)digest)) &&
     chn->data.nan_state.num_payments == chn->data.nan_public.num_payments){
    digestmap_remove(client.nans_estab, (char*)digest);
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = dpay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_reqclose1(chn, &pid);
  }

  // make direct payment if possible; callback pay_finish
  if((chn = digestmap_remove(client.nans_destab, (char*)digest))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = pay_finish, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_dpay1(chn, &pid);
  }

  // establish nanopayment channel if possible; callback dpay_helper
  if((chn = smartlist_idesc_remove(client.nans_setup, rdesc))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->rdesc = *rdesc;
    chn->callback = (mt_callback_t){.fn = dpay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_destab1(chn, &pid);
  }

  // set up nanopayment channel if possible; callback dpay_helper
  if((chn = smartlist_idesc_remove(client.chns_estab, rdesc))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = dpay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_setup1(chn, &pid);
  }

  // establish channel if possible; callback dpay_helper
  if((chn = smartlist_idesc_remove(client.chns_setup, rdesc))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = dpay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_chn_end_estab1(chn, &pid);
  }

  // setup channel if possible; callback dpay_helper
  if(client.mac_balance >= MT_CLI_CHN_VAL + client.fee){
    chn = new_channel();
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->idesc = *idesc;
    chn->callback = (mt_callback_t){.fn = dpay_helper, .dref1 = *rdesc, .dref2 = *idesc};
    return init_chn_end_setup(chn, &pid);
  }

  log_debug(LD_MT, "insufficient funds to create new channel");
  return MT_ERROR;
}

/**
 * Close an existing payment channel with the given relay/intermediary pair
 */
int mt_cpay_close(mt_desc_t* rdesc, mt_desc_t* idesc){
  byte digest[DIGEST_LEN];
  mt_desc2digest(rdesc, &digest);

  byte pid[DIGEST_LEN];
  mt_crypt_rand(DIGEST_LEN, pid);

  mt_channel_t* chn;

  // close the standard nanopayment channel if possible; callback close_finish
  if((chn = digestmap_remove(client.nans_reqclosed, (char*)digest))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = close_finish, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_end_close1(chn, &pid);
  }

  // send a request to close the channel if possible; callback close_helper
  if((chn = digestmap_remove(client.nans_estab, (char*)digest))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = mt_cpay_close, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_cli_reqclose1(chn, &pid);
  }

  // close the direct nanopayment channel if possible; callback close_finish
  if((chn = digestmap_remove(client.nans_destab, (char*)digest))){
    digestmap_set(client.chns_transition, (char*)pid, chn);
    chn->callback = (mt_callback_t){.fn = close_finish, .dref1 = *rdesc, .dref2 = *idesc};
    return init_nan_end_close1(chn, &pid);
  }

  log_debug(LD_MT, "descriptor is in an incorrect state to perform the requested action");
  return MT_ERROR;
}

/**
 * Handle an incoming message from the given descriptor
 */
int mt_cpay_recv(mt_desc_t* desc, mt_ntype_t type, byte* msg, int size){

  int result;
  byte pid[DIGEST_LEN];

  // unpack the token and delegate to appropriate handler
  switch(type){
    case MT_NTYPE_ANY_LED_CONFIRM:;
      any_led_confirm_t any_led_confirm_tkn;
      if(unpack_any_led_confirm(msg, size, &any_led_confirm_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_any_led_confirm(desc, &any_led_confirm_tkn, &pid);
      break;
    case MT_NTYPE_CHN_INT_ESTAB2:;
      chn_int_estab2_t chn_int_estab2_tkn;
      if(unpack_chn_int_estab2(msg, size, &chn_int_estab2_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_chn_int_estab2(desc, &chn_int_estab2_tkn, &pid);
      break;

    case MT_NTYPE_CHN_INT_ESTAB4:;
      chn_int_estab4_t chn_int_estab4_tkn;
      if(unpack_chn_int_estab4(msg, size, &chn_int_estab4_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_chn_int_estab4(desc, &chn_int_estab4_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_SETUP2:;
      nan_int_setup2_t nan_int_setup2_tkn;
      if(unpack_nan_int_setup2(msg, size, &nan_int_setup2_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_setup2(desc, &nan_int_setup2_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_SETUP4:;
      nan_int_setup4_t nan_int_setup4_tkn;
      if(unpack_nan_int_setup4(msg, size, &nan_int_setup4_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_setup4(desc, &nan_int_setup4_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_SETUP6:;
      nan_int_setup6_t nan_int_setup6_tkn;
      if(unpack_nan_int_setup6(msg, size, &nan_int_setup6_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_setup6(desc, &nan_int_setup6_tkn, &pid);
      break;

    case MT_NTYPE_NAN_REL_ESTAB6:;
      nan_rel_estab6_t nan_rel_estab6_tkn;
      if(unpack_nan_rel_estab6(msg, size, &nan_rel_estab6_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_rel_estab6(desc, &nan_rel_estab6_tkn, &pid);
      break;

    case MT_NTYPE_NAN_REL_PAY2:;
      nan_rel_pay2_t nan_rel_pay2_tkn;
      if(unpack_nan_rel_pay2(msg, size, &nan_rel_pay2_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_rel_pay2(desc, &nan_rel_pay2_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_DESTAB2:;
      nan_int_destab2_t nan_int_destab2_tkn;
      if(unpack_nan_int_destab2(msg, size, &nan_int_destab2_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_destab2(desc, &nan_int_destab2_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_DPAY2:;
      nan_int_dpay2_t nan_int_dpay2_tkn;
      if(unpack_nan_int_dpay2(msg, size, &nan_int_dpay2_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_dpay2(desc, &nan_int_dpay2_tkn, &pid);
      break;

    case MT_NTYPE_NAN_REL_REQCLOSE2:;
      nan_rel_reqclose2_t nan_rel_reqclose2_tkn;
      if(unpack_nan_rel_reqclose2(msg, size, &nan_rel_reqclose2_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_rel_reqclose2(desc, &nan_rel_reqclose2_tkn,  &pid);
      break;

    case MT_NTYPE_NAN_INT_CLOSE2:;
      nan_int_close2_t nan_int_close2_tkn;
      if(unpack_nan_int_close2(msg, size, &nan_int_close2_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_close2(desc, &nan_int_close2_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_CLOSE4:;
      nan_int_close4_t nan_int_close4_tkn;
      if(unpack_nan_int_close4(msg, size, &nan_int_close4_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_close4(desc, &nan_int_close4_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_CLOSE6:;
      nan_int_close6_t nan_int_close6_tkn;
      if(unpack_nan_int_close6(msg, size, &nan_int_close6_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_close6(desc, &nan_int_close6_tkn, &pid);
      break;

    case MT_NTYPE_NAN_INT_CLOSE8:;
      nan_int_close8_t nan_int_close8_tkn;
      if(unpack_nan_int_close8(msg, size, &nan_int_close8_tkn, &pid) != MT_SUCCESS)
	return MT_ERROR;
      result = handle_nan_int_close8(desc, &nan_int_close8_tkn, &pid);
      break;

    default:
      result = MT_ERROR;
      break;
  }

  return result;
}

/**
 * Return the balance of available money to spend as macropayments
 */
int mt_cpay_mac_balance(void){
  return client.mac_balance;
}

/**
 * Return the balance of money locked up in channels
 */
int mt_cpay_chn_balance(void){
  return client.chn_balance;
}

/**
 * Return the number of channels currently open
 */
int mt_cpay_chn_number(void){
  return client.chn_number;
}

/**
 * Delete the state of the payment module
 */
int mt_cpay_clear(void){
  // Need to implement
  return 0;
}

/**
 * Export the state of the payment module into a serialized malloc'd byte string
 */
int mt_cpay_export(byte** export_out){
  *export_out = tor_malloc(sizeof(client));
  memcpy(*export_out, &client, sizeof(client));
  return sizeof(client);
}

/**
 * Overwrite the current payment module state with the provided string state
 */
int mt_cpay_import(byte* import){
  memcpy(&client, import, sizeof(client));
  return MT_SUCCESS;
}

/******************************* Channel Setup **************************/

static int init_chn_end_setup(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // initialize setup token

  chn_end_setup_t token;
  token.val_from = MT_CLI_CHN_VAL + client.fee;
  token.val_to = MT_CLI_CHN_VAL;
  token.val_from = 105 * 30 + 5;
  token.val_to = 105 * 30;
  memcpy(token.from, client.addr, MT_SZ_ADDR);
  memcpy(token.chn, chn->data.addr, MT_SZ_ADDR);
  // skip public for now

  // update local data
  client.chn_number ++;
  client.mac_balance -= token.val_from;
  client.chn_balance += token.val_to;
  chn->data.balance = token.val_to;

  // send setup message
  byte* msg;
  byte* signed_msg;
  int msg_size = pack_chn_end_setup(&token, pid, &msg);
  int signed_msg_size = mt_create_signed_msg(msg, msg_size,
					     &chn->data.pk, &chn->data.sk, &signed_msg);
  int result = mt_send_message(&client.ledger, MT_NTYPE_CHN_END_SETUP, signed_msg, signed_msg_size);
  tor_free(msg);
  tor_free(signed_msg);
  return result;
}

static int handle_any_led_confirm(mt_desc_t* desc, any_led_confirm_t* token, byte (*pid)[DIGEST_LEN]){

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  if(desc->id != client.ledger.id || desc->party != MT_PARTY_LED)
    return MT_ERROR;

  if(token->success != MT_CODE_SUCCESS)
    return MT_ERROR;

  digestmap_remove(client.chns_transition, (char*)*pid);
  smartlist_add(client.chns_setup, chn);
  if(chn->callback.fn){
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  }
  return MT_SUCCESS;
}

/****************************** Channel Establish ***********************/

static int init_chn_end_estab1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  mt_wcom_args_t* args = tor_malloc(sizeof(mt_wcom_args_t));
  args->chn = chn;
  memcpy(args->pid, *pid, DIGEST_LEN);

  if(!cpuworker_queue_work(WQ_PRI_HIGH, wcom_task, (work_task)help_chn_end_estab1, args))
    return MT_ERROR;
  return MT_SUCCESS;
}

static int help_chn_end_estab1(void* args){

  // extract parameters
  mt_channel_t* chn = ((mt_wcom_args_t*)args)->chn;
  byte pid[DIGEST_LEN];
  memcpy(pid, ((mt_wcom_args_t*)args)->pid, DIGEST_LEN);
  tor_free(args);

  chn_end_estab1_t token;
  memcpy(token.addr, chn->data.addr, MT_SZ_ADDR);
  // TODO finish making token;

  // send message
  byte* msg;
  int msg_size = pack_chn_end_estab1(&token, &pid, &msg);
  int result =  mt_send_message(&chn->idesc, MT_NTYPE_CHN_END_ESTAB1, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_chn_int_estab2(mt_desc_t* desc, chn_int_estab2_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity incoming message

  chn_end_estab3_t reply;

  // fill reply with correct values

  byte* msg;
  int msg_size = pack_chn_end_estab3(&reply, pid, &msg);
  int result = mt_send_message(desc, MT_NTYPE_CHN_END_ESTAB3, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_chn_int_estab4(mt_desc_t* desc, chn_int_estab4_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity of incoming message;

  // prepare nanopayment channel token now
  mt_wcom_args_t* args = tor_malloc(sizeof(mt_wcom_args_t));
  args->chn = chn;
  memcpy(args->pid, *pid, DIGEST_LEN);

  if(!cpuworker_queue_work(WQ_PRI_HIGH, wcom_task, (work_task)help_chn_int_estab4, args))
    return MT_ERROR;
  return MT_SUCCESS;
}

static int help_chn_int_estab4(void* args){

  // extract parameters
  mt_channel_t* chn = ((mt_wcom_args_t*)args)->chn;
  byte pid[DIGEST_LEN];
  memcpy(pid, ((mt_wcom_args_t*)args)->pid, DIGEST_LEN);
  tor_free(args);

  // save token to channel
  digestmap_remove(client.chns_transition, (char*)pid);
  smartlist_add(client.chns_estab, chn);

  // check validity of incoming message
  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/******************************** Nano Setup ****************************/

static int init_nan_cli_setup1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // TODO: copy over and wpk/npwk/wcom/zkp

  // create hash chain
  byte hc_head[MT_SZ_HASH];
  mt_crypt_rand(MT_SZ_HASH, hc_head);
  mt_hc_create(MT_NAN_LEN, &hc_head, &chn->data.nan_secret.hc);

  // make token
  nan_cli_setup1_t token;
  token.nan_public.val_from = MT_NAN_VAL + (MT_NAN_VAL * client.tax) / 100;
  token.nan_public.val_to = MT_NAN_VAL;
  token.nan_public.num_payments = MT_NAN_LEN;
  memcpy(token.nan_public.hash_tail, chn->data.nan_secret.hc[0], MT_SZ_HASH);

  // update channel data
  memcpy(&chn->data.nan_public, &token.nan_public, sizeof(nan_any_public_t));
  memcpy(chn->data.nan_state.last_hash, chn->data.nan_secret.hc[0], MT_SZ_HASH);
  chn->data.nan_state.num_payments = 0;

  // send message
  byte* msg;
  int msg_size = pack_nan_cli_setup1(&token, pid, &msg);
  int result = mt_send_message(&chn->idesc, MT_NTYPE_NAN_CLI_SETUP1, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_setup2(mt_desc_t* desc, nan_int_setup2_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity incoming message

  nan_cli_setup3_t reply;

  // fill reply with correct values

  byte* msg;
  int msg_size = pack_nan_cli_setup3(&reply, pid, &msg);
  int result = mt_send_message(desc, MT_NTYPE_NAN_CLI_SETUP3, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_setup4(mt_desc_t* desc, nan_int_setup4_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity incoming message

  nan_cli_setup5_t reply;

  // fill reply with correct values

  byte* msg;
  int msg_size = pack_nan_cli_setup5(&reply, pid, &msg);
  int result = mt_send_message(desc, MT_NTYPE_NAN_CLI_SETUP5, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_setup6(mt_desc_t* desc, nan_int_setup6_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  digestmap_remove(client.chns_transition, (char*)*pid);
  smartlist_add(client.nans_setup, chn);

  // sort nans_setup here?

  // check validity incoming message
  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/**************************** Nano Establish ****************************/

static int init_nan_cli_estab1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // make token
  nan_cli_estab1_t token;
  memcpy(&token.nan_public, &chn->data.nan_public, sizeof(nan_any_public_t));

  // send message
  byte* msg;
  int msg_size = pack_nan_cli_estab1(&token, pid, &msg);
  int result = mt_send_message_multidesc(&chn->rdesc, &chn->idesc, MT_NTYPE_NAN_CLI_ESTAB1,
					 msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_rel_estab6(mt_desc_t* desc, nan_rel_estab6_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity incoming message
  byte digest[DIGEST_LEN];
  mt_desc2digest(desc, &digest);
  digestmap_remove(client.chns_transition, (char*)*pid);
  digestmap_set(client.nans_estab, (char*)digest, chn);

  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/******************************* Nano Pay *******************************/

static int init_nan_cli_pay1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // make token
  nan_cli_pay1_t token;

  // TODO finish making setup;

  // update channel data
  client.chn_balance -= chn->data.nan_public.val_from;
  chn->data.balance -= chn->data.nan_public.val_from;
  chn->data.nan_state.num_payments ++;

  // send message
  byte* msg;
  int msg_size = pack_nan_cli_pay1(&token, pid, &msg);
  int result = mt_send_message(&chn->rdesc, MT_NTYPE_NAN_CLI_PAY1, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_rel_pay2(mt_desc_t* desc, nan_rel_pay2_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  byte digest[DIGEST_LEN];
  mt_desc2digest(desc, &digest);
  digestmap_remove(client.chns_transition, (char*)*pid);
  digestmap_set(client.nans_estab, (char*)digest, chn);

  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/************************ Nano Direct Establish *************************/

static int init_nan_cli_destab1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // intiate token
  nan_cli_destab1_t token;
  memcpy(&token.nan_public, &chn->data.nan_public, sizeof(nan_any_public_t));

  // send message
  byte* msg;
  int msg_size = pack_nan_cli_destab1(&token, pid, &msg);
  int result = mt_send_message(&chn->idesc, MT_NTYPE_NAN_CLI_DESTAB1, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_destab2(mt_desc_t* desc, nan_int_destab2_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  byte digest[DIGEST_LEN];
  mt_desc2digest(desc, &digest);
  digestmap_remove(client.chns_transition, (char*)*pid);
  digestmap_set(client.nans_destab, (char*)digest, chn);

  // check validity incoming message

  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/**************************** Nano Direct Pay ***************************/

static int init_nan_cli_dpay1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // intiate token
  nan_cli_dpay1_t token;
  memcpy(&token.nan_public, &chn->data.nan_public, sizeof(nan_any_public_t));

  // TODO finish making setup;

  // update balances
  client.chn_balance -= chn->data.nan_public.val_from;
  chn->data.balance -= chn->data.nan_public.val_from;
  chn->data.nan_state.num_payments ++;

  // send message
  byte* msg;
  int msg_size = pack_nan_cli_dpay1(&token, pid, &msg);
  int result = mt_send_message(&chn->idesc, MT_NTYPE_NAN_CLI_DPAY1, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_dpay2(mt_desc_t* desc, nan_int_dpay2_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  byte digest[DIGEST_LEN];
  mt_desc2digest(desc, &digest);
  digestmap_remove(client.chns_transition, (char*)*pid);
  digestmap_set(client.nans_destab, (char*)digest, chn);

  // check validity incoming message
  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/****************************** Nano Req Close **************************/

static int init_nan_cli_reqclose1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // intiate token
  nan_cli_reqclose1_t token;

  // TODO finish making token;

  // send message
  byte* msg;
  int msg_size = pack_nan_cli_reqclose1(&token, pid, &msg);
  int result = mt_send_message(&chn->rdesc, MT_NTYPE_NAN_CLI_REQCLOSE1, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_rel_reqclose2(mt_desc_t* desc, nan_rel_reqclose2_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  byte digest[DIGEST_LEN];
  mt_desc2digest(desc, &digest);
  digestmap_remove(client.chns_transition, (char*)*pid);
  digestmap_set(client.nans_reqclosed, (char*)digest, chn);

  // check validity incoming message
  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/******************************* Nano Close *****************************/

static int init_nan_end_close1(mt_channel_t* chn, byte (*pid)[DIGEST_LEN]){

  // intiate token
  nan_end_close1_t token;
  token.total_val = chn->data.nan_state.num_payments * chn->data.nan_public.val_from;
  token.num_payments = chn->data.nan_state.num_payments;
  memcpy(&token.nan_public, &chn->data.nan_public, sizeof(nan_any_public_t));

  // TODO finish making token;

  // send message
  byte* msg;
  int msg_size = pack_nan_end_close1(&token, pid, &msg);
  int result = mt_send_message(&chn->idesc, MT_NTYPE_NAN_END_CLOSE1, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_close2(mt_desc_t* desc, nan_int_close2_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity incoming message

  nan_end_close3_t reply;

  // fill reply with correct values

  byte* msg;
  int msg_size = pack_nan_end_close3(&reply, pid, &msg);
  int result = mt_send_message(desc, MT_NTYPE_NAN_END_CLOSE3, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_close4(mt_desc_t* desc, nan_int_close4_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity incoming message

  nan_end_close5_t reply;

  // fill reply with correct values

  byte* msg;
  int msg_size = pack_nan_end_close5(&reply, pid, &msg);
  int result = mt_send_message(desc, MT_NTYPE_NAN_END_CLOSE5, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_close6(mt_desc_t* desc, nan_int_close6_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // check validity incoming message

  nan_end_close7_t reply;

  // fill reply with correct values

  byte* msg;
  int msg_size = pack_nan_end_close7(&reply, pid, &msg);
  int result = mt_send_message(desc, MT_NTYPE_NAN_END_CLOSE7, msg, msg_size);
  tor_free(msg);
  return result;
}

static int handle_nan_int_close8(mt_desc_t* desc, nan_int_close8_t* token, byte (*pid)[DIGEST_LEN]){
  (void)token;
  (void)desc;

  mt_channel_t* chn = digestmap_get(client.chns_transition, (char*)*pid);
  if(chn == NULL){
    log_debug(LD_MT, "protocol id not recognized");
    return MT_ERROR;
  }

  // validate token

  mt_wcom_args_t* args = tor_malloc(sizeof(mt_wcom_args_t));
  args->chn = chn;
  memcpy(args->pid, *pid, DIGEST_LEN);

  if(!cpuworker_queue_work(WQ_PRI_HIGH, wcom_task, (work_task)help_nan_int_close8, args))
    return MT_ERROR;
  return MT_SUCCESS;
}

static int help_nan_int_close8(void* args){
  mt_channel_t* chn = ((mt_wcom_args_t*)args)->chn;
  byte pid[DIGEST_LEN];
  memcpy(pid, ((mt_wcom_args_t*)args)->pid, DIGEST_LEN);
  tor_free(args);

  digestmap_remove(client.chns_transition, (char*)pid);
  //smartlist_add(client.chns_estab, chn);

  // if sufficient funds left then move channel to establish state, otherwise move to spent
  if(chn->data.balance >= MT_NAN_LEN * (MT_NAN_VAL + (MT_NAN_VAL * client.tax) / 100))
    smartlist_add(client.chns_estab, chn);
  else
    smartlist_add(client.chns_spent, chn);

  if(chn->callback.fn)
    return chn->callback.fn(&chn->callback.dref1, &chn->callback.dref2);
  return MT_SUCCESS;
}

/***************************** Helper Functions *************************/

static mt_channel_t* new_channel(void){

  // initialize new channel
  mt_channel_t* chn = tor_calloc(1, sizeof(mt_channel_t));
  memcpy(chn->data.pk, client.pk, MT_SZ_PK);
  memcpy(chn->data.sk, client.sk, MT_SZ_SK);
  mt_crypt_rand(MT_SZ_ADDR, chn->data.addr);

  // TODO finish initializing channel

  return chn;
}

static int compare_chn_end_data(const void** a, const void** b){

  if(((mt_channel_t*)(*a))->data.balance > ((mt_channel_t*)(*b))->data.balance)
    return -1;

  if(((mt_channel_t*)(*a))->data.balance < ((mt_channel_t*)(*b))->data.balance)
    return 1;

  return MT_SUCCESS;
}

static workqueue_reply_t wcom_task(void* thread, void* args){
  (void)thread;

  // extract parameters
  mt_channel_t* chn = ((mt_wcom_args_t*)args)->chn;
  (void)chn;

  // call mt_commit_wallet here
  return WQ_RPL_REPLY;
}

static int pay_finish(mt_desc_t* rdesc, mt_desc_t* idesc){
  return mt_pay_success(rdesc, idesc, MT_SUCCESS);
}

static int close_finish(mt_desc_t* rdesc, mt_desc_t* idesc){
  (void)rdesc;
  (void)idesc;

  smartlist_sort(client.nans_setup, compare_chn_end_data);
  return mt_close_success(rdesc, idesc, MT_SUCCESS);
}

static mt_channel_t* smartlist_idesc_remove(smartlist_t* list, mt_desc_t* desc){

  SMARTLIST_FOREACH_BEGIN(list, mt_channel_t*, elm){
    if(elm->idesc.id == desc->id && elm->idesc.party == desc->party){
      smartlist_remove(list, elm);
      return elm;
    }
  } SMARTLIST_FOREACH_END(elm);
  return NULL;
}