/*
 * miaobox_cmd.c — 控制台命令实现
 * ===============================
 *
 * NVS 存储方案:
 *   每个 key 对应两个 NVS 条目:
 *     <key>      — 值 (字符串或 int32)
 *     <key>_t   — 类型标签 ("str" 或 "int"), 后缀仅2字符适配15字符限制
 *
 * 使用示例:
 *   miao> setcfg wifi.ssid.1 str MyWiFi
 *   miao> setcfg wifi.pwd.1 str pass123
 *   miao> setcfg ntp.timeout int 120000
 *   miao> getcfg wifi.ssid.1
 *   miao> factoryreset
 */

#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "argtable3/argtable3.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "miaobox_cmd.h"

static const char *TAG = "miaobox_cmd";

#define NVS_NAMESPACE  "cfg"
#define MAX_STR_LEN    256  /* max length for string values */

/* ---- helpers ---- */

static nvs_handle_t open_nvs(nvs_open_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, mode, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
    }
    return handle;
}

static void close_nvs(nvs_handle_t handle)
{
    nvs_close(handle);
}

/* Store the type tag alongside the key so getcfg knows what to read */
static esp_err_t set_type_tag(nvs_handle_t handle, const char *key, const char *type_str)
{
    char type_key[MAX_STR_LEN];
    snprintf(type_key, sizeof(type_key), "%s_t", key);
    return nvs_set_str(handle, type_key, type_str);
}

static esp_err_t get_type_tag(nvs_handle_t handle, const char *key, char *type_out, size_t out_len)
{
    char type_key[MAX_STR_LEN];
    snprintf(type_key, sizeof(type_key), "%s_t", key);
    size_t len = out_len;
    esp_err_t err = nvs_get_str(handle, type_key, type_out, &len);
    return err;
}

/* ---- setcfg ---- */

static struct {
    struct arg_str *key;
    struct arg_str *type;
    struct arg_str *str_val;
    struct arg_int *int_val;
    struct arg_end *end;
} setcfg_args;

static int setcfg_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&setcfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, setcfg_args.end, argv[0]);
        return 1;
    }

    const char *key = setcfg_args.key->sval[0];
    const char *type = setcfg_args.type->sval[0];

    nvs_handle_t handle = open_nvs(NVS_READWRITE);
    if (handle == 0) return 1;

    esp_err_t err = ESP_OK;

    if (strcmp(type, "str") == 0) {
        const char *val = setcfg_args.str_val->sval[0];
        err = nvs_set_str(handle, key, val);
        if (err == ESP_OK) {
            err = set_type_tag(handle, key, "str");
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (err == ESP_OK) {
            printf("OK: %s = \"%s\" (str)\n", key, val);
        }
    } else if (strcmp(type, "int") == 0) {
        int32_t val = setcfg_args.int_val->ival[0];
        err = nvs_set_i32(handle, key, val);
        if (err == ESP_OK) {
            err = set_type_tag(handle, key, "int");
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (err == ESP_OK) {
            printf("OK: %s = %" PRId32 " (int)\n", key, val);
        }
    } else {
        printf("Error: unknown type \"%s\". Use str or int.\n", type);
        close_nvs(handle);
        return 1;
    }

    if (err != ESP_OK) {
        printf("Error: %s\n", esp_err_to_name(err));
    }

    close_nvs(handle);
    return (err == ESP_OK) ? 0 : 1;
}

/* ---- getcfg ---- */

static struct {
    struct arg_str *key;
    struct arg_end *end;
} getcfg_args;

static int getcfg_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&getcfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, getcfg_args.end, argv[0]);
        return 1;
    }

    const char *key = getcfg_args.key->sval[0];

    nvs_handle_t handle = open_nvs(NVS_READONLY);
    if (handle == 0) return 1;

    /* Read the type tag first */
    char type_str[8] = {0};
    esp_err_t err = get_type_tag(handle, key, type_str, sizeof(type_str));

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("Key \"%s\" not found\n", key);
        close_nvs(handle);
        return 1;
    }
    if (err != ESP_OK) {
        printf("Error reading type: %s\n", esp_err_to_name(err));
        close_nvs(handle);
        return 1;
    }

    if (strcmp(type_str, "str") == 0) {
        char val[MAX_STR_LEN];
        size_t len = sizeof(val);
        err = nvs_get_str(handle, key, val, &len);
        if (err == ESP_OK) {
            printf("%s = \"%s\" (str)\n", key, val);
        }
    } else if (strcmp(type_str, "int") == 0) {
        int32_t val;
        err = nvs_get_i32(handle, key, &val);
        if (err == ESP_OK) {
            printf("%s = %" PRId32 " (int)\n", key, val);
        }
    } else {
        printf("Unknown type tag \"%s\" for key \"%s\"\n", type_str, key);
        close_nvs(handle);
        return 1;
    }

    if (err != ESP_OK) {
        printf("Error reading value: %s\n", esp_err_to_name(err));
    }

    close_nvs(handle);
    return (err == ESP_OK) ? 0 : 1;
}

/* ---- delcfg ---- */

static struct {
    struct arg_str *key;
    struct arg_end *end;
} delcfg_args;

static int delcfg_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&delcfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, delcfg_args.end, argv[0]);
        return 1;
    }

    const char *key = delcfg_args.key->sval[0];

    nvs_handle_t handle = open_nvs(NVS_READWRITE);
    if (handle == 0) return 1;

    /* Delete value */
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("Key \"%s\" not found\n", key);
        /* Still try to clean up type tag */
    } else if (err != ESP_OK) {
        printf("Error deleting \"%s\": %s\n", key, esp_err_to_name(err));
        close_nvs(handle);
        return 1;
    }

    /* Delete type tag */
    char type_key[MAX_STR_LEN];
    snprintf(type_key, sizeof(type_key), "%s_t", key);
    nvs_erase_key(handle, type_key);  /* ignore error — may not exist */

    err = nvs_commit(handle);
    if (err == ESP_OK) {
        printf("OK: deleted \"%s\"\n", key);
    }

    close_nvs(handle);
    return 0;
}

/* ---- factoryreset ---- */

static int factoryreset_cmd(int argc, char **argv)
{
    printf("Erasing all NVS data...\n");
    nvs_flash_erase();
    printf("NVS erased. Rebooting...\n");
    esp_restart();
    return 0;
}

/* ---- reboot ---- */

static int reboot_cmd(int argc, char **argv)
{
    printf("Rebooting...\n");
    esp_restart();
    return 0;
}

/* ---- registration ---- */

void register_miaobox_commands(void)
{
    /* setcfg <key> <str|int> [value] */
    setcfg_args.key      = arg_str1(NULL, NULL, "<key>",     "Configuration key name");
    setcfg_args.type     = arg_str1(NULL, NULL, "<str|int>", "Value type: str or int");
    setcfg_args.str_val  = arg_str0(NULL, NULL, "<value>",   "String value (when type=str)");
    setcfg_args.int_val  = arg_int0(NULL, NULL, "<value>",   "Integer value (when type=int)");
    setcfg_args.end      = arg_end(2);

    const esp_console_cmd_t setcfg = {
        .command   = "setcfg",
        .help      = "Set a config value: setcfg <key> str <value> | setcfg <key> int <value>",
        .hint      = NULL,
        .func      = &setcfg_cmd,
        .argtable  = &setcfg_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&setcfg));

    /* getcfg <key> */
    getcfg_args.key = arg_str1(NULL, NULL, "<key>", "Configuration key name");
    getcfg_args.end = arg_end(2);

    const esp_console_cmd_t getcfg = {
        .command   = "getcfg",
        .help      = "Get a config value: getcfg <key>",
        .hint      = NULL,
        .func      = &getcfg_cmd,
        .argtable  = &getcfg_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&getcfg));

    /* delcfg <key> */
    delcfg_args.key = arg_str1(NULL, NULL, "<key>", "Configuration key name");
    delcfg_args.end = arg_end(2);

    const esp_console_cmd_t delcfg = {
        .command   = "delcfg",
        .help      = "Delete a config value: delcfg <key>",
        .hint      = NULL,
        .func      = &delcfg_cmd,
        .argtable  = &delcfg_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&delcfg));

    /* factoryreset */
    const esp_console_cmd_t factoryreset = {
        .command   = "factoryreset",
        .help      = "Erase all NVS data and reboot",
        .hint      = NULL,
        .func      = &factoryreset_cmd,
        .argtable  = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&factoryreset));

    /* reboot */
    const esp_console_cmd_t reboot = {
        .command   = "reboot",
        .help      = "Restart the device",
        .hint      = NULL,
        .func      = &reboot_cmd,
        .argtable  = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot));

    ESP_LOGI(TAG, "Registered setcfg / getcfg / delcfg / reboot commands");
}
