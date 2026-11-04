// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ARMDECK_OSK_RIME_H
#define ARMDECK_OSK_RIME_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    void *lib;
    void *api;
    unsigned long session;
    bool ready;
} OskRime;

bool osk_rime_init(OskRime *rime);
void osk_rime_fini(OskRime *rime);
bool osk_rime_key(OskRime *rime, int keysym, bool shift);
bool osk_rime_backspace(OskRime *rime);
bool osk_rime_select(OskRime *rime, int index);
bool osk_rime_commit(OskRime *rime, char *out, size_t out_size);
bool osk_rime_commit_composition(OskRime *rime);
bool osk_rime_context(OskRime *rime, char *preedit, size_t preedit_size,
                      char candidates[][64], int max_candidates,
                      int *count, int *page, bool *last_page);
void osk_rime_reset(OskRime *rime);
void osk_rime_set_chinese(OskRime *rime, bool chinese);
void osk_rime_selftest(OskRime *rime);

#endif
