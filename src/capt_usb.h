/*
 * Persistent USB Driver & CAPT v1.0 State Machine for Canon LASER SHOT LBP-2900
 */

#ifndef CAPT_USB_H
#define CAPT_USB_H

#include "capt_defs.h"

/* Initialize libusb and open persistent connection to Canon LBP2900 (04a9:2676) */
bool capt_usb_init_persistent(void);
void capt_usb_shutdown(void);
bool capt_usb_is_connected(void);
bool capt_usb_verify_connection(int pending_jobs);
void capt_set_pending_jobs_count(int pending_jobs, const char *latest_job_name);
void capt_test_set_simulated_disconnect(bool sim_disc);
void capt_test_set_simulated_eng_fault(uint16_t sim_eng_mask);

/* Poll printer status safely (thread-safe via internal USB mutex) */
bool capt_usb_poll_live_status(struct capt_status_s *out_status);

/* Execute a full print job from a document file using streaming host rendering */
bool capt_execute_print_job(uint32_t job_id,
                            const char *job_name,
                            const char *job_user,
                            const char *filepath,
                            const struct lbp2900_prefs *prefs);

/* Run printer fuser roller cleaning cycle */
bool capt_execute_cleaning(void);

/* Access shared live status for UI / CLI / IPP */
void capt_get_daemon_live_status(struct daemon_live_status *out);
void capt_request_pause(bool pause);
void capt_request_cancel(void);
void capt_request_soft_resume(void);

/* Callback hook invoked whenever UI should auto-popup (job start or out-of-paper) */
typedef void (*ui_popup_hook_fn)(const char *reason);
void capt_set_ui_popup_hook(ui_popup_hook_fn fn);

#endif /* CAPT_USB_H */
