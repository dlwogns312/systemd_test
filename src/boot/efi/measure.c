/* SPDX-License-Identifier: LGPL-2.1-or-later */

#if ENABLE_TPM

#include <efi.h>
#include <efilib.h>

#include "macro-fundamental.h"
#include "measure.h"
#include "missing_efi.h"
#include "util.h"

static EFI_STATUS tpm1_measure_to_pcr_and_event_log(
                const EFI_TCG *tcg,
                uint32_t pcrindex,
                EFI_PHYSICAL_ADDRESS buffer,
                UINTN buffer_size,
                const char16_t *description) {

        _cleanup_free_ TCG_PCR_EVENT *tcg_event = NULL;
        EFI_PHYSICAL_ADDRESS event_log_last;
        uint32_t event_number = 1;
        UINTN desc_len;

        assert(tcg);
        assert(description);

        desc_len = strsize16(description);
        tcg_event = xmalloc(offsetof(TCG_PCR_EVENT, Event) + desc_len);
        memset(tcg_event, 0, offsetof(TCG_PCR_EVENT, Event) + desc_len);
        *tcg_event = (TCG_PCR_EVENT) {
                .EventSize = desc_len,
                .PCRIndex = pcrindex,
                .EventType = EV_IPL,
        };
        memcpy(tcg_event->Event, description, desc_len);

        return tcg->HashLogExtendEvent(
                        (EFI_TCG *) tcg,
                        buffer, buffer_size,
                        TCG_ALG_SHA,
                        tcg_event,
                        &event_number,
                        &event_log_last);
}

static EFI_STATUS tpm2_measure_to_pcr_and_event_log(
                EFI_TCG2 *tcg,
                uint32_t pcrindex,
                EFI_PHYSICAL_ADDRESS buffer,
                uint64_t buffer_size,
                const char16_t *description) {

        _cleanup_free_ EFI_TCG2_EVENT *tcg_event = NULL;
        UINTN desc_len;

        assert(tcg);
        assert(description);

        desc_len = strsize16(description);
        tcg_event = xmalloc(offsetof(EFI_TCG2_EVENT, Event) + desc_len);
        memset(tcg_event, 0, offsetof(EFI_TCG2_EVENT, Event) + desc_len);
        *tcg_event = (EFI_TCG2_EVENT) {
                .Size = offsetof(EFI_TCG2_EVENT, Event) + desc_len,
                .Header.HeaderSize = sizeof(EFI_TCG2_EVENT_HEADER),
                .Header.HeaderVersion = EFI_TCG2_EVENT_HEADER_VERSION,
                .Header.PCRIndex = pcrindex,
                .Header.EventType = EV_IPL,
        };

        memcpy(tcg_event->Event, description, desc_len);

        return tcg->HashLogExtendEvent(
                        tcg,
                        0,
                        buffer, buffer_size,
                        tcg_event);
}

static EFI_TCG *tcg1_interface_check(void) {
        EFI_PHYSICAL_ADDRESS event_log_location, event_log_last_entry;
        TCG_BOOT_SERVICE_CAPABILITY capability = {
                .Size = sizeof(capability),
        };
        EFI_STATUS err;
        uint32_t features;
        EFI_TCG *tcg;

        err = BS->LocateProtocol((EFI_GUID *) EFI_TCG_GUID, NULL, (void **) &tcg);
        if (err != EFI_SUCCESS)
                return NULL;

        err = tcg->StatusCheck(
                        tcg,
                        &capability,
                        &features,
                        &event_log_location,
                        &event_log_last_entry);
        if (err != EFI_SUCCESS)
                return NULL;

        if (capability.TPMDeactivatedFlag)
                return NULL;

        if (!capability.TPMPresentFlag)
                return NULL;

        return tcg;
}

static EFI_TCG2 * tcg2_interface_check(void) {
        EFI_TCG2_BOOT_SERVICE_CAPABILITY capability = {
                .Size = sizeof(capability),
        };
        EFI_STATUS err;
        EFI_TCG2 *tcg;

        err = BS->LocateProtocol((EFI_GUID *) EFI_TCG2_GUID, NULL, (void **) &tcg);
        if (err != EFI_SUCCESS)
                return NULL;

        err = tcg->GetCapability(tcg, &capability);
        if (err != EFI_SUCCESS)
                return NULL;

        if (capability.StructureVersion.Major == 1 &&
            capability.StructureVersion.Minor == 0) {
                TCG_BOOT_SERVICE_CAPABILITY *caps_1_0 =
                        (TCG_BOOT_SERVICE_CAPABILITY*) &capability;
                if (caps_1_0->TPMPresentFlag)
                        return tcg;
        }

        if (!capability.TPMPresentFlag)
                return NULL;

        return tcg;
}

bool tpm_present(void) {
        return tcg2_interface_check() || tcg1_interface_check();
}

EFI_STATUS tpm_log_event(uint32_t pcrindex, EFI_PHYSICAL_ADDRESS buffer, UINTN buffer_size, const char16_t *description) {
        EFI_TCG *tpm1;
        EFI_TCG2 *tpm2;

        assert(description);

        /* PCR disabled */
        if (pcrindex == UINT32_MAX)
                return EFI_SUCCESS;

        tpm2 = tcg2_interface_check();
        if (tpm2)
                return tpm2_measure_to_pcr_and_event_log(tpm2, pcrindex, buffer, buffer_size, description);

        tpm1 = tcg1_interface_check();
        if (tpm1)
                return tpm1_measure_to_pcr_and_event_log(tpm1, pcrindex, buffer, buffer_size, description);

        /* No active TPM found, so don't return an error */
        return EFI_SUCCESS;
}

EFI_STATUS tpm_log_load_options(const char16_t *load_options) {
        EFI_STATUS err;

        /* Measures a load options string into the TPM2, i.e. the kernel command line */

        for (UINTN i = 0; i < 2; i++) {
                uint32_t pcr = i == 0 ? TPM_PCR_INDEX_KERNEL_PARAMETERS : TPM_PCR_INDEX_KERNEL_PARAMETERS_COMPAT;

                err = tpm_log_event(pcr,
                                    POINTER_TO_PHYSICAL_ADDRESS(load_options),
                                    strsize16(load_options), load_options);
                if (err != EFI_SUCCESS)
                        return log_error_status_stall(err, L"Unable to add load options (i.e. kernel command) line measurement to PCR %u: %r", pcr, err);
        }

        return EFI_SUCCESS;
}

#endif
