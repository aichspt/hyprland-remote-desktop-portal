#define _GNU_SOURCE
#include "hrdp.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_DIR "hyprland-remote-desktop-portal"

static char *xdg_path(const char *env, const char *fallback, const char *file)
{
	const char *base = getenv(env);
	const char *home = getenv("HOME");
	char *path = NULL;

	if (base && *base)
		asprintf(&path, "%s/" APP_DIR "/%s", base, file);
	else if (home)
		asprintf(&path, "%s/%s/" APP_DIR "/%s", home, fallback, file);
	return path;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static void add_app(struct config *cfg, const char *app)
{
	if (!*app || config_app_allowed(cfg, app))
		return;
	char **apps = realloc(cfg->allowed_apps, (cfg->n_allowed_apps + 1) * sizeof(*apps));
	if (!apps)
		return;
	cfg->allowed_apps = apps;
	cfg->allowed_apps[cfg->n_allowed_apps++] = strdup(app);
}

static void add_app_list(struct config *cfg, char *list)
{
	for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ","))
		add_app(cfg, trim(tok));
}

static bool parse_bool(const char *v)
{
	return strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0;
}

static void set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = *v ? strdup(v) : NULL;
}

/*
 * Config format, one "key = value" per line, '#' starts a comment:
 *   allow_apps       = org.example.App, com.other.App
 *   ask_unidentified = false
 *   xkb_layout       = us
 *   xkb_variant / xkb_options / xkb_model / xkb_rules
 */
void config_load(struct config *cfg)
{
	char *path = xdg_path("XDG_CONFIG_HOME", ".config", "config");
	FILE *f = path ? fopen(path, "r") : NULL;
	char *line = NULL;
	size_t cap = 0;

	if (f) {
		log_info("reading %s", path);
		while (getline(&line, &cap, f) > 0) {
			char *hash = strchr(line, '#');
			if (hash)
				*hash = '\0';
			char *eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = '\0';
			char *key = trim(line), *val = trim(eq + 1);
			if (strcmp(key, "allow_apps") == 0)
				add_app_list(cfg, val);
			else if (strcmp(key, "ask_unidentified") == 0)
				cfg->ask_unidentified = parse_bool(val);
			else if (strcmp(key, "xkb_rules") == 0)
				set_str(&cfg->xkb_rules, val);
			else if (strcmp(key, "xkb_model") == 0)
				set_str(&cfg->xkb_model, val);
			else if (strcmp(key, "xkb_layout") == 0)
				set_str(&cfg->xkb_layout, val);
			else if (strcmp(key, "xkb_variant") == 0)
				set_str(&cfg->xkb_variant, val);
			else if (strcmp(key, "xkb_options") == 0)
				set_str(&cfg->xkb_options, val);
			else
				log_warn("%s: unknown key '%s'", path, key);
		}
		fclose(f);
	}
	free(path);

	/* apps the user chose "Always allow" for */
	path = xdg_path("XDG_STATE_HOME", ".local/state", "allowed-apps");
	f = path ? fopen(path, "r") : NULL;
	if (f) {
		while (getline(&line, &cap, f) > 0)
			add_app(cfg, trim(line));
		fclose(f);
	}
	free(path);
	free(line);
}

bool config_app_allowed(const struct config *cfg, const char *app_id)
{
	for (size_t i = 0; i < cfg->n_allowed_apps; i++)
		if (strcmp(cfg->allowed_apps[i], app_id) == 0)
			return true;
	return false;
}

static void mkdir_parents(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		mkdir(path, 0700);
		*p = '/';
	}
}

void config_remember_app(struct config *cfg, const char *app_id)
{
	if (!*app_id || config_app_allowed(cfg, app_id))
		return;
	add_app(cfg, app_id);

	char *path = xdg_path("XDG_STATE_HOME", ".local/state", "allowed-apps");
	if (!path)
		return;
	mkdir_parents(path);
	FILE *f = fopen(path, "a");
	if (f) {
		fprintf(f, "%s\n", app_id);
		fclose(f);
	} else {
		log_warn("cannot write %s: %s", path, strerror(errno));
	}
	free(path);
}
