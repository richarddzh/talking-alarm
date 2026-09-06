#include "app_secret_store.h"

#include <stdio.h>
#include <string.h>

static app_secrets_t s_current;

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

bool app_secrets_valid(const app_secrets_t *s) {
    return s && s->asr_api_key[0] && s->agent_api_key[0] && s->tts_api_key[0];
}

int app_secrets_load_partial(app_secrets_t *out) {
    if (!out) return -2;
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(APP_SECRETS_CONFIG_PATH, "r");
    if (!f) return -1;
    fgets(out->asr_api_key, sizeof(out->asr_api_key), f);
    fgets(out->agent_api_key, sizeof(out->agent_api_key), f);
    fgets(out->tts_api_key, sizeof(out->tts_api_key), f);
    fclose(f);
    rstrip(out->asr_api_key);
    rstrip(out->agent_api_key);
    rstrip(out->tts_api_key);
    return 0;
}

int app_secrets_load(app_secrets_t *out) {
    int rc = app_secrets_load_partial(out);
    if (rc != 0) return rc;
    return app_secrets_valid(out) ? 0 : -2;
}

int app_secrets_save_partial(const app_secrets_t *in) {
    if (!in) return -2;
    FILE *f = fopen(APP_SECRETS_CONFIG_PATH, "w");
    if (!f) return -2;
    fprintf(f, "%s\n%s\n%s\n", in->asr_api_key, in->agent_api_key, in->tts_api_key);
    fclose(f);
    return 0;
}

int app_secrets_save(const app_secrets_t *in) {
    if (!app_secrets_valid(in)) return -2;
    return app_secrets_save_partial(in);
}

void app_secrets_set_current(const app_secrets_t *s) {
    if (s) s_current = *s;
    else memset(&s_current, 0, sizeof(s_current));
}

bool app_secrets_ready(void) { return app_secrets_valid(&s_current); }
const char *app_secrets_asr_api_key(void) { return s_current.asr_api_key; }
const char *app_secrets_agent_api_key(void) { return s_current.agent_api_key; }
const char *app_secrets_tts_api_key(void) { return s_current.tts_api_key; }
