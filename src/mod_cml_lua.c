#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "mod_cml.h"
#include "mod_cml_funcs.h"
#include "log.h"
#include "stream.h"

#ifdef USE_OPENSSL
# include <openssl/md5.h>
#else
# include "md5_global.h"
# include "md5.h"
#endif

#define HASHLEN 16
typedef unsigned char HASH[HASHLEN];
#define HASHHEXLEN 32
typedef char HASHHEX[HASHHEXLEN+1];
#ifdef USE_OPENSSL
#define IN const
#else
#define IN 
#endif
#define OUT

#ifdef HAVE_LUA_H

#include <lua.h>
#include <lualib.h>

typedef struct {
	stream st;
	int done;
} readme;

static const char * load_file(lua_State *L, void *data, size_t *size) {
	readme *rm = data;
	
	UNUSED(L);
	
	if (rm->done) return 0;
	
	*size = rm->st.size;
	rm->done = 1;
	return rm->st.start;
}

static int lua_to_c_get_string(lua_State *L, const char *varname, buffer *b) {
	int curelem;
	
	lua_pushstring(L, varname);
	
	curelem = lua_gettop(L);
	lua_gettable(L, LUA_GLOBALSINDEX);
	
	/* it should be a table */
	if (!lua_isstring(L, curelem)) {
		lua_settop(L, curelem - 1);
		
		return -1;
	}
	
	buffer_copy_string(b, lua_tostring(L, curelem));
	
	lua_pop(L, 1);
	
	assert(curelem - 1 == lua_gettop(L));
	
	return 0;
}

static int lua_to_c_is_table(lua_State *L, const char *varname) {
	int curelem;
	
	lua_pushstring(L, varname);
	
	curelem = lua_gettop(L);
	lua_gettable(L, LUA_GLOBALSINDEX);
	
	/* it should be a table */
	if (!lua_istable(L, curelem)) {
		lua_settop(L, curelem - 1);
		
		return 0;
	}
	
	lua_settop(L, curelem - 1);
	
	assert(curelem - 1 == lua_gettop(L));
	
	return 1;
}

static int c_to_lua_push(lua_State *L, int tbl, const char *key, size_t key_len, const char *val, size_t val_len) {
	lua_pushlstring(L, key, key_len);
	lua_pushlstring(L, val, val_len);
	lua_settable(L, tbl);
	
	return 0;
}


int split_query_string(lua_State *L, int tbl, buffer *qrystr) {
	size_t is_key = 1;
	size_t i;
	char *key = NULL, *val = NULL;
	
	key = qrystr->ptr;
	
	/* we need the \0 */
	for (i = 0; i < qrystr->used; i++) {
		switch(qrystr->ptr[i]) {
		case '=':
			if (is_key) {
				val = qrystr->ptr + i + 1;
				
				qrystr->ptr[i] = '\0';
				
				is_key = 0;
			}
			
			break;
		case '&':
		case '\0': /* fin symbol */
			if (!is_key) {
				/* we need at least a = since the last & */
				
				c_to_lua_push(L, tbl, 
					      key, strlen(key),
					      val, strlen(val));
			}
			
			key = qrystr->ptr + i + 1;
			val = NULL;
			is_key = 1;
			break;
		}
	}
	
	return 0;
}

int cache_parse_lua(server *srv, connection *con, plugin_data *p, buffer *fn) {
	lua_State *L; 
	readme rm;
	int ret = -1;
	buffer *b = buffer_init();
	int header_tbl = 0;
	
	rm.done = 0;
	stream_open(&rm.st, fn);
	
	/* push the lua file to the interpreter and see what happends */
	L = lua_open();
	
	luaopen_base(L);
	luaopen_table(L);
	luaopen_string(L);
	luaopen_math(L);
	luaopen_io(L);
	
	/* register functions */
	lua_register(L, "md5", f_crypto_md5);
	lua_register(L, "file_mtime", f_file_mtime);
	lua_register(L, "file_isreg", f_file_isreg);
	lua_register(L, "file_isdir", f_file_isreg);
	lua_register(L, "dir_files", f_dir_files);
	
#ifdef HAVE_MEMCACHE_H
	lua_pushliteral(L, "memcache_get_long");
	lua_pushlightuserdata(L, p->conf.mc);
	lua_pushcclosure(L, f_memcache_get_long, 1);
	lua_settable(L, LUA_GLOBALSINDEX);
	
	lua_pushliteral(L, "memcache_get_string");
	lua_pushlightuserdata(L, p->conf.mc);
	lua_pushcclosure(L, f_memcache_get_string, 1);
	lua_settable(L, LUA_GLOBALSINDEX);
	
	lua_pushliteral(L, "memcache_exists");
	lua_pushlightuserdata(L, p->conf.mc);
	lua_pushcclosure(L, f_memcache_exists, 1);
	lua_settable(L, LUA_GLOBALSINDEX);
#endif
	/* register CGI environment */
	lua_pushliteral(L, "request");
	lua_newtable(L);
	lua_settable(L, LUA_GLOBALSINDEX);
	
	lua_pushliteral(L, "request");
	header_tbl = lua_gettop(L);
	lua_gettable(L, LUA_GLOBALSINDEX);
	
	c_to_lua_push(L, header_tbl, CONST_STR_LEN("REQUEST_URI"), CONST_BUF_LEN(con->request.orig_uri));
	c_to_lua_push(L, header_tbl, CONST_STR_LEN("SCRIPT_NAME"), CONST_BUF_LEN(con->uri.path));
	c_to_lua_push(L, header_tbl, CONST_STR_LEN("SCRIPT_FILENAME"), CONST_BUF_LEN(con->physical.path));
	c_to_lua_push(L, header_tbl, CONST_STR_LEN("DOCUMENT_ROOT"), CONST_BUF_LEN(con->physical.doc_root));
	if (!buffer_is_empty(con->request.pathinfo)) {
		c_to_lua_push(L, header_tbl, CONST_STR_LEN("PATH_INFO"), CONST_BUF_LEN(con->request.pathinfo));
	}
	
	c_to_lua_push(L, header_tbl, CONST_STR_LEN("CWD"), CONST_BUF_LEN(p->basedir));
	c_to_lua_push(L, header_tbl, CONST_STR_LEN("BASEURL"), CONST_BUF_LEN(p->baseurl));
	
	/* register GET parameter */
	lua_pushliteral(L, "get");
	lua_newtable(L);
	lua_settable(L, LUA_GLOBALSINDEX);
	
	lua_pushliteral(L, "get");
	header_tbl = lua_gettop(L);
	lua_gettable(L, LUA_GLOBALSINDEX);
	
	
	buffer_copy_string_buffer(b, con->uri.query);
	split_query_string(L, header_tbl, b);
	buffer_reset(b);
	
	lua_pushliteral(L, "CACHE_HIT");
	lua_pushnumber(L, 0);
	lua_settable(L, LUA_GLOBALSINDEX);
	
	lua_pushliteral(L, "CACHE_MISS");
	lua_pushnumber(L, 1);
	lua_settable(L, LUA_GLOBALSINDEX);
	
	/* load lua program */
	if (lua_load(L, load_file, &rm, fn->ptr) || lua_pcall(L,0,1,0)) {
		log_error_write(srv, __FILE__, __LINE__, "s",
				lua_tostring(L,-1));
		
		goto error;
	}
	
	/* get return value */
	ret = (int)lua_tonumber(L, -1);
	lua_pop(L, 1);
	
	/* fetch the data from lua */ 
	lua_to_c_get_string(L, "trigger_handler", p->trigger_handler);
	
	if (0 == lua_to_c_get_string(L, "output_contenttype", b)) {
		response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(b));
	}
	
	if (!lua_to_c_is_table(L, "output_include")) {
		log_error_write(srv, __FILE__, __LINE__, "s",
				"output_include is missing or not a table");
		ret = -1;
		
		goto error;
	}
	
	if (ret == 0) {
		/* up to now it is a cache-hit, check if all files exist */
		
		struct stat st;
		int curelem;
		time_t mtime = 0;
		
		lua_pushstring(L, "output_include");
		
		curelem = lua_gettop(L);
		lua_gettable(L, LUA_GLOBALSINDEX);

		/* HOW-TO build a etag ?
		 * as we don't just have one file we have to take the stat() 
		 * from all base files, merge them and build the etag from
		 * it later.
		 * 
		 * The mtime of the content is the mtime of the freshest base file
		 * 
		 * */
		
		lua_pushnil(L);  /* first key */
		while (lua_next(L, curelem) != 0) {
			/* key' is at index -2 and value' at index -1 */
			
			if (lua_isstring(L, -1)) {
				buffer_copy_string_buffer(b, p->basedir);
				buffer_append_string(b, lua_tostring(L, -1));
				
				if (0 != stat(b->ptr, &st)) {
					/* stat failed */
					
					if (errno == ENOENT) {
						/* a file is missing, call the handler to generate it */
						if (!buffer_is_empty(p->trigger_handler)) {
							ret = 1; /* cache-miss */
							
							log_error_write(srv, __FILE__, __LINE__, "s",
									"a file is missing, calling handler");
							
							break;
						} else {
							/* handler not set -> 500 */
							ret = -1;
							
							log_error_write(srv, __FILE__, __LINE__, "s",
									"a file missing and no handler set");
							
							break;
						}
					}
				} else {
					chunkqueue_append_file(con->write_queue, b, 0, st.st_size);
					if (st.st_mtime > mtime) mtime = st.st_mtime;
				}
			} else {
				/* not a string */
				ret = -1;
				log_error_write(srv, __FILE__, __LINE__, "s",
						"not a string");
				break;
			}
		
			lua_pop(L, 1);  /* removes value'; keeps key' for next iteration */
		}
		
		lua_settop(L, curelem - 1);
		
		if (ret == 0) {
			data_string *ds;
			char timebuf[sizeof("Sat, 23 Jul 2005 21:20:01 GMT")];
			buffer tbuf;

			con->file_finished = 1;

			ds = (data_string *)array_get_element(con->response.headers, "Last-Modified");

			/* no Last-Modified specified */
			if ((mtime) && (NULL == ds)) {
		
				strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&mtime));
				
				response_header_overwrite(srv, con, CONST_STR_LEN("Last-Modified"), timebuf, sizeof(timebuf) - 1);


				tbuf.ptr = timebuf;
				tbuf.used = sizeof(timebuf);
				tbuf.size = sizeof(timebuf);
			} else if (ds) {
				tbuf.ptr = ds->value->ptr;
				tbuf.used = ds->value->used;
				tbuf.size = ds->value->size;
			}
			
			if (http_response_handle_cachable(srv, con, &tbuf)) {
				/* ok, the client already has our content, 
				 * no need to send it again */
				chunkqueue_reset(con->write_queue);
			}
		} else {
			chunkqueue_reset(con->write_queue);
		}
	}
	
	if (ret == 1 && buffer_is_empty(p->trigger_handler)) {
		log_error_write(srv, __FILE__, __LINE__, "s",
				"cache-miss, but not trigger_handler set");
		ret = -1;
	}
	
	if (ret == 1) {
		/* cache-miss */
		buffer_copy_string_buffer(con->uri.path, p->baseurl);
		buffer_append_string_buffer(con->uri.path, p->trigger_handler);
	
		buffer_copy_string_buffer(con->physical.path, p->basedir);
		buffer_append_string_buffer(con->physical.path, p->trigger_handler);
		
		chunkqueue_reset(con->write_queue);
	}
	
error:
	lua_close(L);
	
	stream_close(&rm.st);
	buffer_free(b);
	
	return ret /* cache-error */;
}
#else
int cache_parse_lua(server *srv, connection *con, plugin_data *p, buffer *fn) {
	/* error */
	return -1;
}
#endif
