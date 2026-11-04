// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "rime_engine.h"
#include "rime_api.h"

#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef RimeApi *(*RimeGetApiFn)(void);

static RimeApi *api_of(OskRime *rime)
{
    return (RimeApi *)rime->api;
}

bool osk_rime_init(OskRime *rime)
{
    memset(rime, 0, sizeof(*rime));
    rime->lib = dlopen("librime.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!rime->lib) rime->lib = dlopen("librime.so", RTLD_NOW | RTLD_LOCAL);
    if (!rime->lib) return false;
    RimeGetApiFn get_api = (RimeGetApiFn)dlsym(rime->lib, "rime_get_api");
    if (!get_api) return false;
    rime->api = get_api();
    if (!rime->api) return false;
    RimeApi *api = api_of(rime);
    if (!api->data_size || !api->initialize || !api->create_session) return false;

    const char *override = getenv("TAIJI_RIME_USER_DIR");
    const char *home = getenv("HOME");
    static char userdir[1024];
    if (override && *override) snprintf(userdir, sizeof(userdir), "%s", override);
    else if (home && *home) snprintf(userdir, sizeof(userdir), "%s/.local/share/armdeck-osk/rime", home);
    else snprintf(userdir, sizeof(userdir), "/tmp/armdeck-osk-rime");
    mkdir("/usr/share/rime-data", 0755);
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s", userdir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(userdir, 0755);
    snprintf(tmp, sizeof(tmp), "%s/build", userdir);
    mkdir(tmp, 0755);
    DIR *bd = opendir("/usr/share/rime-data/build");
    if (bd) {
        struct dirent *de;
        while ((de = readdir(bd)) != NULL) {
            const char *n = de->d_name;
            size_t nl = strlen(n);
            if (nl < 4 || (strcmp(n + nl - 4, ".bin") != 0 &&
                           strcmp(n + nl - 5, ".yaml") != 0))
                continue;
            char src[1200], dst[1200];
            snprintf(src, sizeof(src), "/usr/share/rime-data/build/%s", n);
            snprintf(dst, sizeof(dst), "%s/%s", tmp, n);
            struct stat st;
            if (lstat(dst, &st) == 0) {
                if (!S_ISLNK(st.st_mode)) continue;
                unlink(dst);
            }
            FILE *in = fopen(src, "rb");
            if (!in) continue;
            FILE *out = fopen(dst, "wb");
            if (!out) { fclose(in); continue; }
            char buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
                fwrite(buf, 1, got, out);
            fclose(out);
            fclose(in);
            chmod(dst, 0644);
        }
        closedir(bd);
    }

    RimeTraits traits = {0};
    RIME_STRUCT_INIT(RimeTraits, traits);
    traits.shared_data_dir = "/usr/share/rime-data";
    traits.user_data_dir = userdir;
    traits.prebuilt_data_dir = "/usr/share/rime-data/build";
    snprintf(tmp, sizeof(tmp), "%s/build", userdir);
    traits.staging_dir = strdup(tmp);
    traits.distribution_name = "Taiji OSK";
    traits.distribution_code_name = "armdeck-osk";
    traits.distribution_version = "1.0.0";
    traits.app_name = "rime.armdeck-osk";
    api->setup(&traits);
    api->initialize(&traits);
    if (api->start_maintenance) {
        api->start_maintenance(True);
        if (api->join_maintenance_thread) api->join_maintenance_thread();
    }
    rime->session = api->create_session();
    if (!rime->session) return false;
    if (api->set_option) api->set_option(rime->session, "ascii_mode", False);
    rime->ready = true;
    return true;
}

void osk_rime_fini(OskRime *rime)
{
    RimeApi *api = api_of(rime);
    if (api) {
        if (api->destroy_session && rime->session) api->destroy_session(rime->session);
        if (api->finalize) api->finalize();
    }
    if (rime->lib) dlclose(rime->lib);
    memset(rime, 0, sizeof(*rime));
}

void osk_rime_reset(OskRime *rime)
{
    RimeApi *api = api_of(rime);
    if (!rime->ready || !api || !api->clear_composition) return;
    api->clear_composition(rime->session);
}

void osk_rime_set_chinese(OskRime *rime, bool chinese)
{
    RimeApi *api = api_of(rime);
    if (!rime->ready || !api || !api->set_option) return;
    api->set_option(rime->session, "ascii_mode", chinese ? False : True);
}

bool osk_rime_key(OskRime *rime, int keysym, bool shift)
{
    RimeApi *api = api_of(rime);
    if (!rime->ready || !api || !api->process_key) return false;
    (void)shift;
    return api->process_key(rime->session, keysym, 0);
}

bool osk_rime_backspace(OskRime *rime)
{
    RimeApi *api = api_of(rime);
    if (!rime->ready || !api || !api->process_key) return false;
    return api->process_key(rime->session, 0xFF08, 0);
}

bool osk_rime_select(OskRime *rime, int index)
{
    RimeApi *api = api_of(rime);
    if (!rime->ready || !api || !api->select_candidate) return false;
    return api->select_candidate(rime->session, (size_t)index);
}

bool osk_rime_commit(OskRime *rime, char *out, size_t out_size)
{
    RimeApi *api = api_of(rime);
    if (out && out_size) out[0] = 0;
    if (!rime->ready || !api || !api->get_commit) return false;
    RimeCommit commit = {0};
    RIME_STRUCT_INIT(RimeCommit, commit);
    if (!api->get_commit(rime->session, &commit)) return false;
    if (commit.text && out && out_size)
        snprintf(out, out_size, "%s", commit.text);
    if (api->free_commit) api->free_commit(&commit);
    return out && out[0];
}

bool osk_rime_commit_composition(OskRime *rime)
{
    RimeApi *api = api_of(rime);
    if (!rime->ready || !api || !api->commit_composition) return false;
    return api->commit_composition(rime->session);
}

bool osk_rime_context(OskRime *rime, char *preedit, size_t preedit_size,
                      char candidates[][64], int max_candidates,
                      int *count, int *page, bool *last_page)
{
    if (preedit && preedit_size) preedit[0] = 0;
    if (count) *count = 0;
    if (page) *page = 0;
    if (last_page) *last_page = true;
    RimeApi *api = api_of(rime);
    if (!rime->ready || !api || !api->get_context) return false;
    RimeContext ctx = {0};
    RIME_STRUCT_INIT(RimeContext, ctx);
    if (!api->get_context(rime->session, &ctx)) return false;
    if (ctx.composition.preedit && preedit && preedit_size)
        snprintf(preedit, preedit_size, "%s", ctx.composition.preedit);
    int n = ctx.menu.num_candidates;
    if (n > max_candidates) n = max_candidates;
    for (int i = 0; i < n; i++) {
        if (ctx.menu.candidates[i].text)
            snprintf(candidates[i], 64, "%s", ctx.menu.candidates[i].text);
        else
            candidates[i][0] = 0;
    }
    if (count) *count = n;
    if (page) *page = ctx.menu.page_no;
    if (last_page) *last_page = ctx.menu.is_last_page ? true : false;
    if (api->free_context) api->free_context(&ctx);
    return true;
}

void osk_rime_selftest(OskRime *rime)
{
    const char *keys = "nihao";
    for (const char *p = keys; *p; p++)
        osk_rime_key(rime, *p, false);
    char pre[256] = "", cand[8][64] = {{0}};
    int count = 0, page = 0;
    bool last = true;
    osk_rime_context(rime, pre, sizeof(pre), cand, 8, &count, &page, &last);
    fprintf(stderr, "rime preedit=[%s] count=%d page=%d last=%d\n", pre, count, page, last);
    for (int i = 0; i < count; i++) fprintf(stderr, "cand%d=[%s]\n", i, cand[i]);
    if (count > 0) {
        osk_rime_select(rime, 0);
        char out[256] = "";
        osk_rime_commit(rime, out, sizeof(out));
        fprintf(stderr, "commit=[%s]\n", out);
    }
}
