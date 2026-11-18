/*
 * What the rest of the kernel calls in a driver that is left out of it (CONFIG_HAKIOS_<NAME> not set: drivers/<subsystem>/conf,
 * docs/drivers-config.md), the way XNU keeps bsd/kern/bsd_stubs.c for its own parts that a configuration leaves out.
 *
 * The callers are the places that start the drivers (hakios.input.c, hakios.audio.c, bsd_init.c), that stop them on
 * the way back to the loader or out of a panic (loader_return.c, the thermal and battery guards, machine_routines.c)
 * and the few that ask a driver something on the way (is this device one of the storage's, what the battery's
 * temperature is). Each of them calls the driver unconditionally, as it did when every driver was always built; here,
 * for a driver that is not, is what each call then does: nothing, and the answer "no", "not known" or "not there".
 * A driver that is built gets none of this: every block below is for one driver, and empty when that one is in.
 *
 * What is not here, on purpose: a call from one driver to another that it cannot work without. That is a `requires`
 * of its conf (SOUND requires ADSP and SPEAKER, MODEM requires QMI and UFS, ...), and tools/hakios-config.py refuses
 * the configuration that would need a stand-in for it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/hakios.net.h>
#include <sys/hakios.virtio_blk.h>

/* the ends of a run (loader_return.c, the guards), which have no header of their own */
void hakios_adsp_stop(void);
void hakios_adsp_quiesce(void);
void hakios_mpss_stop(void);
void hakios_mpss_quiesce(void);
void hakios_ddr_quiesce(void);
void hakios_ufs_quiesce(void);
void hakios_var_save_on_return(void);
void hakios_mic_quiesce(void);
void HakiosWLANStart(const char *interface, const uint8_t *address);

#ifndef HAKIOS_VIRTIO_INPUT
void hakios_virtio_input_init(void) {}
#endif

#ifndef HAKIOS_GOODIX
void hakios_goodix_init(void) {}
#endif

#ifndef HAKIOS_BUTTONS
void hakios_buttons_init(void) {}
#endif

#ifndef HAKIOS_UFS
void hakios_ufs_init(void) {}
void hakios_ufs_report(__unused const char *when) {}
bool hakios_ufs_dev(__unused dev_t dev) { return false; }
void hakios_ufs_quiesce(void) {}
void hakios_var_save_on_return(void) {}         /* no flash, so no data volume to save into it */
#endif

#ifndef HAKIOS_VIRTIO_BLK
void hakios_virtio_blk_init(void) {}
bool hakios_virtio_blk_dev(__unused dev_t dev) { return false; }
#endif

#ifndef HAKIOS_BATTERY
void hakios_battery_init(void) {}
int hakios_battery_temperature(void) { return -273; }      /* "not known", as the driver says before its first reading */
void hakios_battery_report(__unused const struct hakios_battery_state *state) {}
#endif

/* the POCO F4's amplifiers (devices/munch/drivers/audio), which hakios.sound.c plays on that phone */
#ifndef HAKIOS_MUNCH_AMP
bool hakios_munch_amp_prepare(__unused const uint8_t *cnt, __unused uint64_t size) { return false; }
void hakios_munch_amp_slots(__unused uint32_t bits) {}
bool hakios_munch_amp_start(void) { return false; }
bool hakios_munch_amp_volume(__unused uint32_t below, __unused uint32_t step_ms) { return false; }
bool hakios_munch_amp_healthy(void) { return false; }
void hakios_munch_amp_scale(__unused int16_t *frames, __unused uint32_t count) {}
void hakios_munch_amp_stop(void) {}
void hakios_munch_amp_quiesce(void) {}
#endif

/* the POCO F4's QCA6390 on PCIe and MHI (devices/munch/drivers/wifi), which hakios.wifi.c and hakios.lx.c run on that phone */
#ifndef HAKIOS_MUNCH_WIFI
int hakios_munch_wifi_up(void) { return ENODEV; }
void hakios_munch_wifi_down(void) {}
void hakios_munch_wifi_quiesce(void) {}
void *hakios_munch_wifi_registers(unsigned long *size) { if (size != NULL) { *size = 0; } return NULL; }
int hakios_munch_qrtr_send(__unused const void *data, __unused uint32_t length) { return ENODEV; }
#endif

/* an SM8250 phone's charger, read by the buttons' driver for the POCO F4's fuel gauge */
#ifndef HAKIOS_BUTTONS
bool hakios_munch_charger(__unused uint8_t *input_status, __unused uint8_t *port_kind, __unused uint8_t *charger_state) { return false; }
#endif

#ifndef HAKIOS_CPUFREQ
void hakios_cpufreq_init(void) {}
void hakios_cpufreq_hot(bool hot) { (void)hot; }      /* (the thermal driver's slowing down: nothing to slow) */
void hakios_ddr_quiesce(void) {}
#endif

#ifndef HAKIOS_ADRENO
void hakios_adreno_init(void) {}
void hakios_adreno_quiesce(void) {}
void hakios_adreno_stop(void) {}
void hakios_adreno_minute(void) {}
void hakios_adreno_hot(bool hot) { (void)hot; }
#endif

#ifndef HAKIOS_WIFI
void hakios_wifi_init(void) {}
void hakios_wifi_stop(void) {}
void hakios_wifi_quiesce(void) {}
errno_t hakios_apple80211_ioctl(__unused u_long command, __unused void *data) { return EOPNOTSUPP; }
void HakiosWLANStart(__unused const char *interface, __unused const uint8_t *address) {}
#endif

#ifndef HAKIOS_ADSP
void hakios_adsp_init(void) {}
void hakios_adsp_stop(void) {}
void hakios_adsp_quiesce(void) {}
#endif

#ifndef HAKIOS_MODEM
void hakios_mpss_init(void) {}
void hakios_mpss_stop(void) {}
void hakios_mpss_quiesce(void) {}
#endif

#ifndef HAKIOS_VIRTIO_NET
void hakios_virtio_net_init(void) {}
#endif

#ifndef HAKIOS_VIRTIO_SOUND
void hakios_virtio_sound_init(void) {}
#endif

#ifndef HAKIOS_SPEAKER
void hakios_speaker_init(void) {}
#endif

#ifndef HAKIOS_SOUND
void hakios_sound_dsp_up(void) {}
void hakios_sound_dsp_down(void) {}
void hakios_sound_quiesce(void) {}
#endif

#ifndef HAKIOS_MIC
void hakios_mic_dsp_up(void) {}
void hakios_mic_dsp_down(void) {}
void hakios_mic_quiesce(void) {}
#endif

#ifndef HAKIOS_HAPTIC
void hakios_haptic_init(void) {}
void hakios_haptic_quiesce(void) {}
void hakios_haptic_pretend(__unused uint32_t peak) {}
#endif

#ifndef HAKIOS_TORCH
void hakios_torch_init(void) {}
void hakios_torch_quiesce(void) {}
#endif

#ifndef HAKIOS_CAMERA
void hakios_camera_init(void) {}
void hakios_camera_quiesce(void) {}
#endif
