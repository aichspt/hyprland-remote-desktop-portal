#define _GNU_SOURCE
#include "hrdp.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Reads a string option from Hyprland, e.g. "input:kb_layout". */
static char *hyprland_option(const char *option)
{
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "hyprctl getoption %s 2>/dev/null", option);
	FILE *p = popen(cmd, "r");
	if (!p)
		return NULL;

	char *line = NULL, *value = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, p)) > 0) {
		/* output looks like "str: us" */
		if (strncmp(line, "str: ", 5) != 0)
			continue;
		if (line[n - 1] == '\n')
			line[--n] = '\0';
		const char *v = line + 5;
		if (*v && strcmp(v, "[[EMPTY]]") != 0)
			value = strdup(v);
		break;
	}
	free(line);
	pclose(p);
	return value;
}

static char *pick(const char *configured, const char *hypr_option)
{
	if (configured)
		return strdup(configured);
	return hyprland_option(hypr_option);
}

struct xkb_keymap *keymap_compile(struct xkb_context *ctx, const struct config *cfg)
{
	char *rules = pick(cfg->xkb_rules, "input:kb_rules");
	char *model = pick(cfg->xkb_model, "input:kb_model");
	char *layout = pick(cfg->xkb_layout, "input:kb_layout");
	char *variant = pick(cfg->xkb_variant, "input:kb_variant");
	char *options = pick(cfg->xkb_options, "input:kb_options");

	struct xkb_rule_names names = {
		.rules = rules, .model = model, .layout = layout, .variant = variant, .options = options,
	};
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (km)
		log_info("keymap: layout=%s variant=%s options=%s", layout ? layout : "(default)",
			 variant ? variant : "", options ? options : "");
	else
		log_error("failed to compile the keymap for layout '%s'", layout ? layout : "(default)");

	free(rules);
	free(model);
	free(layout);
	free(variant);
	free(options);
	return km;
}

int keymap_to_fd(struct xkb_keymap *km, size_t *size)
{
	char *text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!text)
		return -1;

	*size = strlen(text) + 1; /* receivers expect the terminating NUL */
	int fd = memfd_create("hrdp-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0)
		goto fail;
	size_t off = 0;
	while (off < *size) {
		ssize_t w = write(fd, text + off, *size - off);
		if (w <= 0)
			goto fail;
		off += (size_t)w;
	}
	fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
	free(text);
	return fd;

fail:
	log_error("cannot create keymap memfd");
	if (fd >= 0)
		close(fd);
	free(text);
	return -1;
}
