/*
MASTER_LIST.C

The server browser asks OpenCE's master servers for their lists of dedicated
servers (master_list.h, and OpenCE's docs/master_protocol.md). A list is
taken only when the master's pinned key signed it, and each game in it is
taken only when its host signed the listing, the same check a game heard on
the brokers gets (p2p_lobby.c).
*/

#include "platform.h"
#include "port_config.h"
#include "master_list.h"
#include "p2p_internal.h"
#include "update.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
	ROUND_SECONDS = 30,
	FRESH_SECONDS = 300,
	MAXIMUM_MASTERS = 4,
	KEY_SIZE = 32,
	URL_SIZE = 256,
	MAXIMUM_ANSWER = 1024 * 1024,
	MAXIMUM_LISTING = 400,
	MAXIMUM_ENTRIES = 512,
};

#if defined(HALO_IOS) || (defined(HALO_ANDROID) && !defined(HALO_MACOS))

int host_https_get(const char *url, int *status, char *headers, int headers_capacity, char *body, int body_capacity,
	int *body_size)
{
	(void)url;
	(void)status;
	(void)headers;
	(void)headers_capacity;
	(void)body;
	(void)body_capacity;
	(void)body_size;
	return 0;
}

void master_list_browse(int on)
{
	(void)on;
}

void master_list_refresh(void)
{
}

#else

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t wake;
	unsigned long generation;
	int browsing;
	int refresh;
	char urls[MAXIMUM_MASTERS][URL_SIZE];
	unsigned char keys[MAXIMUM_MASTERS][KEY_SIZE];
	int count;
} list = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.wake = PTHREAD_COND_INITIALIZER,
};

static void wait_seconds(int seconds)
{
	struct timespec until;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += seconds;
	pthread_cond_timedwait(&list.wake, &list.lock, &until);
}

static int hex_digit(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

static int master_url_allowed(const char *url)
{
	return !strncmp(url, "https://", 8);
}

static int master_keep(const char *url, const char *key, char (*urls)[URL_SIZE], unsigned char (*keys)[KEY_SIZE],
	int index)
{
	int digit;

	if (!master_url_allowed(url) || strlen(url) >= URL_SIZE || strncmp(key, "ed25519:", 8) ||
		strlen(key) != 8 + 2 * KEY_SIZE)
	{
		return 0;
	}
	for (digit = 0; digit < KEY_SIZE; digit++)
	{
		int high = hex_digit(key[8 + 2 * digit]);
		int low = hex_digit(key[8 + 2 * digit + 1]);

		if (high < 0 || low < 0)
			return 0;
		keys[index][digit] = (unsigned char)(high << 4 | low);
	}
	snprintf(urls[index], URL_SIZE, "%s", url);
	while (strlen(urls[index]) > 8 && urls[index][strlen(urls[index]) - 1] == '/')
		urls[index][strlen(urls[index]) - 1] = 0;
	return 1;
}

/* the official master, asked when masters.txt names none */
static int read_defaults(char (*urls)[URL_SIZE], unsigned char (*keys)[KEY_SIZE], int maximum)
{
	static const char url[] = "https://master.opencommunityedition.com";
	static const char key[] = "ed25519:89f4eda71229f11f3538679f49ef7fb0c2b407b380093f7f4fe02105b92225aa";

	if (maximum < 1)
		return 0;
	return master_keep(url, key, urls, keys, 0);
}

static int read_file(char (*urls)[URL_SIZE], unsigned char (*keys)[KEY_SIZE], int maximum)
{
	const char *setting = config_string("network.masters_file");
	char folder[1024];
	char path[1200];
	char *text;
	const char *line;
	size_t length;
	int count = 0;

	if (!setting || !setting[0])
		return 0;
	if (setting[0] == '/' || (setting[0] && setting[1] == ':'))
	{
		snprintf(path, sizeof(path), "%s", setting);
	}
	else
	{
		config_folder(folder, sizeof(folder));
		snprintf(path, sizeof(path), "%s%s", folder, setting);
	}
	text = config_file_read(path, &length);
	if (!text)
		return 0;
	for (line = text; *line && count < maximum;)
	{
		const char *end = line + strcspn(line, "\n");
		char copy[512];
		char url[URL_SIZE];
		char key[96];

		snprintf(copy, sizeof(copy), "%.*s", (int)(end - line), line);
		copy[strcspn(copy, "#\r")] = 0;
		line = *end ? end + 1 : end;
		if (sscanf(copy, "%255s %95s", url, key) != 2)
			continue;
		if (master_keep(url, key, urls, keys, count))
			count++;
	}
	free(text);
	return count;
}

static void read_masters(void)
{
	char urls[MAXIMUM_MASTERS][URL_SIZE];
	unsigned char keys[MAXIMUM_MASTERS][KEY_SIZE];
	int count = read_file(urls, keys, MAXIMUM_MASTERS);

	if (!count)
		count = read_defaults(urls, keys, MAXIMUM_MASTERS);
	pthread_mutex_lock(&list.lock);
	memcpy(list.urls, urls, sizeof(urls));
	memcpy(list.keys, keys, sizeof(keys));
	list.count = count;
	pthread_mutex_unlock(&list.lock);
}

/* ---------- the list's JSON, only as far as the fields that are read */

struct span
{
	const char *p;
	const char *end;
};

static void skip_ws(struct span *span)
{
	while (span->p < span->end && (*span->p == ' ' || *span->p == '\t' || *span->p == '\n' || *span->p == '\r'))
		span->p++;
}

static int peek(struct span *span)
{
	skip_ws(span);
	return span->p < span->end ? *span->p : 0;
}

static int take(struct span *span, char character)
{
	if (peek(span) != character)
		return 0;
	span->p++;
	return 1;
}

static int skip_string(struct span *span)
{
	if (!take(span, '"'))
		return 0;
	while (span->p < span->end)
	{
		char character = *span->p++;

		if (character == '"')
			return 1;
		if (character == '\\')
		{
			if (span->p >= span->end)
				return 0;
			if (*span->p == 'u')
			{
				int index;

				span->p++;
				for (index = 0; index < 4; index++)
				{
					if (span->p >= span->end || hex_digit(*span->p) < 0)
						return 0;
					span->p++;
				}
			}
			else
			{
				span->p++;
			}
		}
	}
	return 0;
}

static int parse_string(struct span *span, char *text, int size)
{
	int length = 0;

	if (!take(span, '"'))
		return 0;
	while (span->p < span->end)
	{
		char character = *span->p++;

		if (character == '"')
		{
			if (length >= size)
				return 0;
			text[length] = 0;
			return 1;
		}
		if (character == '\\')
		{
			if (span->p >= span->end)
				return 0;
			character = *span->p++;
			if (character == 'u')
			{
				int index;

				for (index = 0; index < 4; index++)
				{
					if (span->p >= span->end || hex_digit(*span->p) < 0)
						return 0;
					span->p++;
				}
				character = '?';
			}
			else if (character == 'n')
				character = '\n';
			else if (character == 't')
				character = '\t';
			else if (character != '"' && character != '\\' && character != '/')
				return 0;
		}
		if (length + 1 >= size)
			return 0;
		text[length++] = character;
	}
	return 0;
}

static int parse_number(struct span *span, double *number)
{
	char token[64];
	char *after;
	int length = 0;

	skip_ws(span);
	while (span->p < span->end && length < (int)sizeof(token) - 1)
	{
		char character = *span->p;

		if (!((character >= '0' && character <= '9') || character == '-' || character == '+' || character == '.' ||
			character == 'e' || character == 'E'))
		{
			break;
		}
		token[length++] = character;
		span->p++;
	}
	token[length] = 0;
	if (!length)
		return 0;
	*number = strtod(token, &after);
	return after != token && !*after;
}

static int skip_value(struct span *span);

static int skip_container(struct span *span, char open, char close)
{
	int first = 1;

	if (!take(span, open))
		return 0;
	while (peek(span) != close)
	{
		if (!peek(span))
			return 0;
		if (!first && !take(span, ','))
			return 0;
		first = 0;
		if (open == '{')
		{
			if (!skip_string(span) || !take(span, ':') || !skip_value(span))
				return 0;
		}
		else if (!skip_value(span))
		{
			return 0;
		}
	}
	return take(span, close);
}

static int skip_value(struct span *span)
{
	int character = peek(span);

	if (character == '"')
		return skip_string(span);
	if (character == '{')
		return skip_container(span, '{', '}');
	if (character == '[')
		return skip_container(span, '[', ']');
	if (character == 't')
	{
		span->p += 4;
		return span->p <= span->end;
	}
	if (character == 'f')
	{
		span->p += 5;
		return span->p <= span->end;
	}
	if (character == 'n')
	{
		span->p += 4;
		return span->p <= span->end;
	}
	{
		double number;

		return parse_number(span, &number);
	}
}

static int parse_bool(struct span *span, int *value)
{
	if (peek(span) == 't')
	{
		span->p += 4;
		*value = 1;
		return span->p <= span->end;
	}
	if (peek(span) == 'f')
	{
		span->p += 5;
		*value = 0;
		return span->p <= span->end;
	}
	return 0;
}

static int base64_digit(char character)
{
	if (character >= 'A' && character <= 'Z')
		return character - 'A';
	if (character >= 'a' && character <= 'z')
		return character - 'a' + 26;
	if (character >= '0' && character <= '9')
		return character - '0' + 52;
	if (character == '+')
		return 62;
	if (character == '/')
		return 63;
	return -1;
}

static int base64_decode(const char *text, unsigned char *out, int out_size)
{
	int length = 0;
	int count = 0;
	int value = 0;

	while (*text)
	{
		int digit;

		if (*text == '=')
			break;
		digit = base64_digit(*text++);
		if (digit < 0)
			return -1;
		value = (value << 6) | digit;
		count++;
		if (count == 4)
		{
			if (length + 3 > out_size)
				return -1;
			out[length++] = (unsigned char)(value >> 16);
			out[length++] = (unsigned char)(value >> 8);
			out[length++] = (unsigned char)value;
			value = 0;
			count = 0;
		}
	}
	if (count == 2)
	{
		if (length + 1 > out_size)
			return -1;
		out[length++] = (unsigned char)(value >> 4);
	}
	else if (count == 3)
	{
		if (length + 2 > out_size)
			return -1;
		out[length++] = (unsigned char)(value >> 10);
		out[length++] = (unsigned char)(value >> 2);
	}
	else if (count != 0)
	{
		return -1;
	}
	return length;
}

static int header_value(const char *headers, const char *name, char *value, int size)
{
	size_t length = strlen(name);
	const char *line = headers;

	value[0] = 0;
	while (line && *line)
	{
		size_t index;
		int match = 1;

		for (index = 0; index < length; index++)
		{
			if (!line[index] || tolower((unsigned char)line[index]) != tolower((unsigned char)name[index]))
			{
				match = 0;
				break;
			}
		}
		if (match && line[length] == ':')
		{
			const char *start = line + length + 1;
			int count;

			while (*start == ' ' || *start == '\t')
				start++;
			count = (int)strcspn(start, "\r\n");
			if (count >= size)
				count = size - 1;
			memcpy(value, start, (size_t)count);
			value[count] = 0;
			return 1;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return 0;
}

/* 1 when the body is a fresh list signed by this key */
static int list_verify(const char *body, int size, const char *signature_header, const char *key_header,
	const unsigned char *key, unsigned long now)
{
	unsigned char signature[P2P_SIGNATURE_SIZE];
	unsigned char digest[P2P_SHA256_SIZE];
	char key_id[2 * 8 + 1];
	char text[96];
	struct span span;
	double protocol = 0, time_value = 0;
	int saw_protocol = 0, saw_key = 0, saw_time = 0;
	int first = 1;
	int decoded;

	p2p_sha256(key, KEY_SIZE, digest);
	p2p_hex(digest, 8, key_id);
	if (!signature_header || strncmp(signature_header, "ed25519=", 8))
		return 0;
	decoded = base64_decode(signature_header + 8, signature, (int)sizeof(signature));
	if (decoded != P2P_SIGNATURE_SIZE)
		return 0;
	if (!key_header || strcmp(key_header, key_id))
		return 0;
	if (!p2p_ed25519_verify(key, body, size, signature))
		return 0;
	span.p = body;
	span.end = body + size;
	if (!take(&span, '{'))
		return 0;
	while (peek(&span) != '}')
	{
		char name[32];

		if ((!first && !take(&span, ',')) || !parse_string(&span, name, (int)sizeof(name)) || !take(&span, ':'))
			return 0;
		first = 0;
		if (!strcmp(name, "protocol"))
		{
			if (!parse_number(&span, &protocol))
				return 0;
			saw_protocol = 1;
		}
		else if (!strcmp(name, "master"))
		{
			int master_first = 1;

			if (!take(&span, '{'))
				return 0;
			while (peek(&span) != '}')
			{
				char field[32];

				if ((!master_first && !take(&span, ',')) || !parse_string(&span, field, (int)sizeof(field)) ||
					!take(&span, ':'))
				{
					return 0;
				}
				master_first = 0;
				if (!strcmp(field, "key_id"))
				{
					if (!parse_string(&span, text, (int)sizeof(text)))
						return 0;
					saw_key = !strcmp(text, key_id);
				}
				else if (!strcmp(field, "time"))
				{
					if (!parse_number(&span, &time_value))
						return 0;
					saw_time = 1;
				}
				else if (!skip_value(&span))
				{
					return 0;
				}
			}
			if (!take(&span, '}'))
				return 0;
		}
		else if (!skip_value(&span))
		{
			return 0;
		}
	}
	if (!take(&span, '}') || !saw_protocol || protocol != 1.0 || !saw_key || !saw_time)
		return 0;
	if ((double)now - time_value > FRESH_SECONDS || time_value - (double)now > FRESH_SECONDS)
		return 0;
	return 1;
}

/* one server object. 1 if its listing was given to the browser */
static int take_server(struct span *span)
{
	char listing_text[600];
	unsigned char listing[MAXIMUM_LISTING];
	int listing_size;
	int dedicated = 0;
	int saw_listing = 0;
	int first = 1;

	listing_text[0] = 0;
	if (!take(span, '{'))
		return 0;
	while (peek(span) != '}')
	{
		char name[32];

		if ((!first && !take(span, ',')) || !parse_string(span, name, (int)sizeof(name)) || !take(span, ':'))
			return -1;
		first = 0;
		if (!strcmp(name, "listing"))
		{
			if (!parse_string(span, listing_text, (int)sizeof(listing_text)))
				return -1;
			saw_listing = 1;
		}
		else if (!strcmp(name, "dedicated"))
		{
			if (!parse_bool(span, &dedicated))
				return -1;
		}
		else if (!skip_value(span))
		{
			return -1;
		}
	}
	if (!take(span, '}'))
		return -1;
	if (!saw_listing)
		return 0;
	listing_size = base64_decode(listing_text, listing, (int)sizeof(listing));
	if (listing_size <= 0)
		return 0;
	return p2p_lobby_take_master_listing(listing, listing_size, dedicated) ? 1 : 0;
}

static int take_list(const char *body, int size)
{
	struct span span;
	int taken = 0;
	int first = 1;
	int index;

	span.p = body;
	span.end = body + size;
	if (!take(&span, '{'))
		return 0;
	while (peek(&span) != '}')
	{
		char name[32];

		if ((!first && !take(&span, ',')) || !parse_string(&span, name, (int)sizeof(name)) || !take(&span, ':'))
			return taken;
		first = 0;
		if (strcmp(name, "servers") || !take(&span, '['))
		{
			if (!skip_value(&span))
				return taken;
			continue;
		}
		index = 0;
		while (peek(&span) != ']' && index < MAXIMUM_ENTRIES)
		{
			int one;

			if (index && !take(&span, ','))
				return taken;
			one = take_server(&span);
			if (one < 0)
				return taken;
			taken += one;
			index++;
		}
	}
	return taken;
}

static int list_current(unsigned long generation)
{
	return list.browsing && list.generation == generation;
}

static void *list_thread_main(void *argument)
{
	unsigned long generation = (unsigned long)(uintptr_t)argument;
	char urls[MAXIMUM_MASTERS][URL_SIZE];
	unsigned char keys[MAXIMUM_MASTERS][KEY_SIZE];
	int count;

	read_masters();
	pthread_mutex_lock(&list.lock);
	while (list_current(generation))
	{
		int index;

		list.refresh = 0;
		memcpy(urls, list.urls, sizeof(urls));
		memcpy(keys, list.keys, sizeof(keys));
		count = list.count;
		pthread_mutex_unlock(&list.lock);
		for (index = 0; index < count; index++)
		{
			char url[URL_SIZE + 32];
			char *headers;
			char *body;
			int status = 0, body_size = 0, taken = 0;

			headers = malloc(8192);
			body = malloc(MAXIMUM_ANSWER + 1);
			snprintf(url, sizeof(url), "%s/v1/servers", urls[index]);
			if (!headers || !body || !host_https_get(url, &status, headers, 8192, body, MAXIMUM_ANSWER + 1, &body_size))
			{
				platform_log("server browser: no answer from %s", urls[index]);
			}
			else if (status != 200)
			{
				platform_log("server browser: %s answered %d", urls[index], status);
			}
			else
			{
				char signature[160];
				char key_header[40];

				header_value(headers, "Signature", signature, (int)sizeof(signature));
				header_value(headers, "Signature-Key", key_header, (int)sizeof(key_header));
				if (!list_verify(body, body_size, signature, key_header, keys[index], (unsigned long)time(NULL)))
					platform_log("server browser: %s's list was refused", urls[index]);
				else
				{
					taken = take_list(body, body_size);
					platform_log("server browser: %s lists %d server%s", urls[index], taken, taken == 1 ? "" : "s");
				}
			}
			free(headers);
			free(body);
			pthread_mutex_lock(&list.lock);
			if (!list_current(generation))
			{
				pthread_mutex_unlock(&list.lock);
				return NULL;
			}
			pthread_mutex_unlock(&list.lock);
		}
		pthread_mutex_lock(&list.lock);
		if (list_current(generation) && !list.refresh)
			wait_seconds(ROUND_SECONDS);
	}
	pthread_mutex_unlock(&list.lock);
	return NULL;
}

void master_list_browse(int on)
{
	pthread_mutex_lock(&list.lock);
	if (on && !list.browsing)
	{
		pthread_t thread;

		list.browsing = 1;
		list.generation++;
		if (pthread_create(&thread, NULL, list_thread_main, (void *)(uintptr_t)list.generation) == 0)
			pthread_detach(thread);
		else
			list.browsing = 0;
		pthread_mutex_unlock(&list.lock);
		return;
	}
	if (!on && list.browsing)
	{
		list.browsing = 0;
		pthread_cond_broadcast(&list.wake);
	}
	pthread_mutex_unlock(&list.lock);
}

void master_list_refresh(void)
{
	pthread_mutex_lock(&list.lock);
	list.refresh = 1;
	pthread_cond_broadcast(&list.wake);
	pthread_mutex_unlock(&list.lock);
}

#endif
