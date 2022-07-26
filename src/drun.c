#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fts.h>
#include <glib.h>
#include <gio/gdesktopappinfo.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "drun.h"
#include "history.h"
#include "log.h"
#include "mkdirp.h"
#include "string_vec.h"
#include "xmalloc.h"

static const char *default_data_dir = ".local/share/";
static const char *default_cache_dir = ".cache/";
static const char *cache_basename = "tofi-drun";

[[nodiscard("memory leaked")]]
static char *get_cache_path() {
	char *cache_name = NULL;
	const char *state_path = getenv("XDG_CACHE_HOME");
	if (state_path == NULL) {
		const char *home = getenv("HOME");
		if (home == NULL) {
			log_error("Couldn't retrieve HOME from environment.\n");
			return NULL;
		}
		size_t len = strlen(home) + 1
			+ strlen(default_cache_dir) + 1
			+ strlen(cache_basename) + 1;
		cache_name = xmalloc(len);
		snprintf(
			cache_name,
			len,
			"%s/%s/%s",
			home,
			default_cache_dir,
			cache_basename);
	} else {
		size_t len = strlen(state_path) + 1
			+ strlen(cache_basename) + 1;
		cache_name = xmalloc(len);
		snprintf(
			cache_name,
			len,
			"%s/%s",
			state_path,
			cache_basename);
	}
	return cache_name;
}

[[nodiscard("memory leaked")]]
static struct string_vec get_application_paths() {
	char *base_paths = NULL;
	const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if (xdg_data_dirs == NULL) {
		xdg_data_dirs = "/usr/local/share/:/usr/share/";
	}
	const char *xdg_data_home = getenv("XDG_DATA_HOME");
	if (xdg_data_home == NULL) {
		const char *home = getenv("HOME");
		if (home == NULL) {
			log_error("Couldn't retrieve HOME from environment.\n");
			exit(EXIT_FAILURE);
		}
		size_t len = strlen(home) + 1
			+ strlen(default_data_dir) + 1
			+ strlen(xdg_data_dirs) + 1;
		base_paths = xmalloc(len);
		snprintf(
			base_paths,
			len,
			"%s/%s:%s",
			home,
			default_data_dir,
			xdg_data_dirs);
	} else {
		size_t len = strlen(xdg_data_home) + 1
			+ strlen(xdg_data_dirs) + 1;
		base_paths = xmalloc(len);
		snprintf(
			base_paths,
			len,
			"%s:%s",
			xdg_data_home,
			xdg_data_dirs);
	}


	/* Append /applications/ to each entry. */
	struct string_vec paths = string_vec_create();
	char *saveptr = NULL;
	char *path_entry = strtok_r(base_paths, ":", &saveptr);
	while (path_entry != NULL) {
		const char *subdir = "applications/";
		size_t len = strlen(path_entry) + 1 + strlen(subdir) + 1;
		char *apps = xmalloc(len);
		snprintf(apps, len, "%s/%s", path_entry, subdir);
		string_vec_add(&paths, apps);
		free(apps);
		path_entry = strtok_r(NULL, ":", &saveptr);
	}
	free(base_paths);

	return paths;
}

static void parse_desktop_file(gpointer key, gpointer value, void *data)
{
	const char *id = key;
	const char *path = value;
	struct desktop_vec *apps = data;

	desktop_vec_add_file(apps, id, path);
}

struct desktop_vec drun_generate(void)
{
	/*
	 * Note for the future: this custom logic could be replaced with
	 * g_app_info_get_all(), but that's slower. Worth remembering
	 * though if this runs into issues.
	 */
	log_debug("Retrieving application dirs.\n");
	struct string_vec paths = get_application_paths();
 	struct string_vec desktop_files = string_vec_create();
 	log_debug("Scanning for .desktop files.\n");
 	for (size_t i = 0; i < paths.count; i++) {
		const char *path_entry = paths.buf[i].string;
 		DIR *dir = opendir(path_entry);
 		if (dir != NULL) {
 			struct dirent *d;
 			while ((d = readdir(dir)) != NULL) {
				const char *extension = strrchr(d->d_name, '.');
				if (extension == NULL) {
					continue;
				}
				if (strcmp(extension, ".desktop")) {
					continue;
				}
 				string_vec_add(&desktop_files, d->d_name);
 			}
 			closedir(dir);
 		}
 	}
	log_debug("Found %zu files.\n", desktop_files.count);


 	log_debug("Parsing .desktop files.\n");
	/*
	 * The Desktop Entry Specification says that only the highest
	 * precedence application file with a given ID should be used, so store
	 * the id / path pairs into a hash table to enforce uniqueness.
	 */
	GHashTable *id_hash = g_hash_table_new_full(g_str_hash, g_str_equal, free, free);
	struct desktop_vec apps = desktop_vec_create();
 	for (size_t i = 0; i < paths.count; i++) {
		char *path_entry = paths.buf[i].string;
		char *tree[2] = { path_entry, NULL };
		size_t prefix_len = strlen(path_entry);
		FTS *fts = fts_open(tree, FTS_LOGICAL, NULL);
		FTSENT *entry = fts_read(fts);
		for (; entry != NULL; entry = fts_read(fts)) {
			const char *extension = strrchr(entry->fts_name, '.');
			if (extension == NULL) {
				continue;
			}
			if (strcmp(extension, ".desktop")) {
				continue;
			}
			char *id = xstrdup(&entry->fts_path[prefix_len]);
			char *slash = strchr(id, '/');
			while (slash != NULL) {
				*slash = '-';
				slash = strchr(slash, '/');
			}
			/*
			 * We're iterating from highest to lowest precedence,
			 * so only the first file with a given ID should be
			 * stored.
			 */
			if (!g_hash_table_contains(id_hash, id)) {
				char *path = xstrdup(entry->fts_path);
				g_hash_table_insert(id_hash, id, path);
			} else {
				free(id);
			}

		}
		fts_close(fts);
 	}

	/* Parse the remaining files into our desktop_vec. */
	g_hash_table_foreach(id_hash, parse_desktop_file, &apps);
	g_hash_table_unref(id_hash);

	log_debug("Found %zu apps.\n", apps.count);

	/*
	 * It's now safe to sort the desktop file vector, as the rules about
	 * file precedence have been taken care of.
	 */
	log_debug("Sorting results.\n");
	desktop_vec_sort(&apps);

	string_vec_destroy(&desktop_files);
	string_vec_destroy(&paths);
	return apps;
}

struct desktop_vec drun_generate_cached()
{
	log_debug("Retrieving cache location.\n");
	char *cache_path = get_cache_path();

	struct stat sb;
	if (cache_path == NULL) {
		return drun_generate();
	}

	/* If the cache doesn't exist, create it and return */
	errno = 0;
	if (stat(cache_path, &sb) == -1) {
		if (errno == ENOENT) {
			struct desktop_vec apps = drun_generate();
			if (!mkdirp(cache_path)) {
				free(cache_path);
				return apps;
			}
			FILE *cache = fopen(cache_path, "wb");
			desktop_vec_save(&apps, cache);
			fclose(cache);
			free(cache_path);
			return apps;
		}
		free(cache_path);
		return drun_generate();
	}

	log_debug("Retrieving application dirs.\n");
	struct string_vec application_path = get_application_paths();;

	/* The cache exists, so check if it's still in date */
	bool out_of_date = false;
	for (size_t i = 0; i < application_path.count; i++) {
		struct stat path_sb;
		if (stat(application_path.buf[i].string, &path_sb) == 0) {
			if (path_sb.st_mtim.tv_sec > sb.st_mtim.tv_sec) {
				out_of_date = true;
				break;
			}
		}
	}
	string_vec_destroy(&application_path);

	struct desktop_vec apps;
	if (out_of_date) {
		log_debug("Cache out of date, updating.\n");
		log_indent();
		apps = drun_generate();
		log_unindent();
		FILE *cache = fopen(cache_path, "wb");
		desktop_vec_save(&apps, cache);
		fclose(cache);
	} else {
		log_debug("Cache up to date, loading.\n");
		FILE *cache = fopen(cache_path, "rb");
		apps = desktop_vec_load(cache);
		fclose(cache);
	}
	free(cache_path);
	return apps;
}

void drun_launch(const char *filename)
{
	GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(filename);
	GAppLaunchContext *context = g_app_launch_context_new();

	if (!g_app_info_launch((GAppInfo *)info, NULL, context, NULL)) {
		log_error("Failed to launch %s.\n", filename);
	}

	g_object_unref(context);
	g_object_unref(info);
}
