 /*
  * fb.c
  * A module to access the Firebird database from Ruby.
  * Fork of interbase.c to fb.c by Brent Rowland.
  * All changes, improvements and associated bugs Copyright (C) 2006 Brent Rowland and Target Training International.
  * License to all changes, improvements and bugs is granted under the same terms as the original and/or
  * the Ruby license, whichever is most applicable.
  * Based on interbase.c
  *
  *                               Copyright (C) 1999 by NaCl inc.
  *                               Copyright (C) 1997,1998 by RIOS Corporation
  *
  * Permission to use, copy, modify, and distribute this software and its
  * documentation for any purpose and without fee is hereby granted, provided
  * that the above copyright notice appear in all copies.
  * RIOS Corporation makes no representations about the suitability of
  * this software for any purpose.  It is provided "as is" without express
  * or implied warranty.  By use of this software the user agrees to
  * indemnify and hold harmless RIOS Corporation from any  claims or
  * liability for loss arising out of such use.
  */

#include "ruby.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ibase.h>
#include <float.h>
#include <time.h>

#define	SQLDA_COLSINIT	10
#define	SQLCODE_NOMORE	100
#define	TPBBUFF_ALLOC	64
#define	CMND_DELIMIT	" \t\n\r\f"
#define	LIST_DELIMIT	", \t\n\r\f"
#define	META_NAME_MAX	31

/* Statement type */
#define	STATEMENT_DDL	1
#define	STATEMENT_DML	0

/* Execute process flag */
#define	EXECF_EXECDML	0
#define	EXECF_SETPARM	1

static VALUE rb_mFb;
static VALUE rb_cFbDatabase;
static VALUE rb_cFbConnection;
static VALUE rb_cFbCursor;
static VALUE rb_eFbError;
static VALUE rb_sFbField;

static char isc_info_stmt[] = { isc_info_sql_stmt_type };
static char isc_info_buff[16];
static char isc_tpb_0[] = {
    isc_tpb_version1,		isc_tpb_write,
    isc_tpb_concurrency,	isc_tpb_nowait
};

/* structs */

/* DB handle and TR parameter block list structure */
typedef struct
{
	isc_db_handle	*dbb_ptr ;
	long		 tpb_len ;
	char		*tpb_ptr ;
} ISC_TEB ; /* transaction existence block */

/* InterBase varchar structure */
typedef	struct
{
	short vary_length;
	char  vary_string[1];
} VARY;

struct FbConnection {
	isc_db_handle db;		/* DB handle */
	VALUE cursor;
	unsigned short dialect;
	unsigned short db_dialect;
	int dropped;
	struct FbConnection *next;
};

static struct FbConnection *fb_connection_list;

struct FbCursor {
	int open;
	isc_stmt_handle stmt;
	VALUE fields_ary;
	VALUE fields_hash;
	VALUE connection;
};

typedef struct trans_opts
{
	char *option1;
	char *option2;
	char  optval;
	short position;
	struct trans_opts *sub_opts;
} trans_opts;

/* global data */
static isc_tr_handle transact = 0;	/* transaction handle */
static XSQLDA *i_sqlda = 0;
static XSQLDA *o_sqlda = 0;
static char *results = 0;
static long  ressize = 0;
static char *paramts = 0;
static long  prmsize = 0;
static int db_num = 0;

/* global utilities */

#define	ALIGN(n, b)	((n + b - 1) & ~(b - 1))
#define	UPPER(c)	(((c) >= 'a' && (c)<= 'z') ? (c) - 'a' + 'A' : (c))
#define	FREE(p)		if (p)	{ free(p); p = 0; }
#define	SETNULL(p)	if (p && strlen(p) == 0)	{ p = 0; }

static long calculate_buffsize(XSQLDA *sqlda)
{
	XSQLVAR *var;
	long cols;
	short dtp;
	long offset;
	long alignment;
	long length;
	long count;

	cols = sqlda->sqld;
	for (var = sqlda->sqlvar, offset = 0,count = 0; count < cols; var++,count++) {
		length = alignment = var->sqllen;
		dtp = var->sqltype & ~1;

		if (dtp == SQL_TEXT) {
			alignment = 1;
		} else if (dtp == SQL_VARYING) {
			length += sizeof(short);
			alignment = sizeof(short);
		}

		offset = ALIGN(offset, alignment);
		offset += length;
		offset = ALIGN(offset, sizeof(short));
		offset += sizeof(short);
	}

	return offset;
}

static VALUE fb_error_msg(long *isc_status)
{
	char msg[512];
	VALUE result = rb_str_new(NULL, 0);
	while (isc_interprete(msg, &isc_status))
	{
		result = rb_str_cat(result, msg, strlen(msg));
		result = rb_str_cat(result, "\n", strlen("\n"));
	}
	return result;
}

static VALUE fb_mktime(struct tm *tm)
{
	return rb_funcall(
		rb_cTime, rb_intern("utc"), 6,
		INT2FIX(tm->tm_year), INT2FIX(tm->tm_mon), INT2FIX(tm->tm_mday),
		INT2FIX(tm->tm_hour), INT2FIX(tm->tm_min), INT2FIX(tm->tm_sec));
}

static VALUE fb_sql_type_from_code(int code, int subtype)
{
	char *sql_type = NULL;
	switch(code) {
		case SQL_TEXT:		sql_type = "CHAR";		break;
		case SQL_VARYING:	sql_type = "VARCHAR";	break;
		case SQL_SHORT:
			switch (subtype) {
				case 0:		sql_type = "SMALLINT";	break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
		case SQL_LONG:
			switch (subtype) {
				case 0:		sql_type = "INTEGER";	break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
		case SQL_FLOAT:		sql_type = "FLOAT";		break;
		case SQL_DOUBLE:
			switch (subtype) {
				case 0:		sql_type = "DOUBLE PRECISION"; break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
		case SQL_D_FLOAT:	sql_type = "DOUBLE PRECISION"; break;
		case SQL_TIMESTAMP:	sql_type = "TIMESTAMP";	break;
		case SQL_BLOB:		sql_type = "BLOB";		break;
		case SQL_ARRAY:		sql_type = "ARRAY";		break;
		case SQL_QUAD:		sql_type = "DECIMAL";	break;
		case SQL_TYPE_TIME:	sql_type = "TIME";		break;
		case SQL_TYPE_DATE:	sql_type = "DATE";		break;
		case SQL_INT64:
			switch (subtype) {
				case 0:		sql_type = "BIGINT";	break;
				case 1:		sql_type = "NUMERIC";	break;
				case 2:		sql_type = "DECIMAL";	break;
			}
			break;
		default:			sql_type = "UNKNOWN";	break;
	}
	return rb_str_new2(sql_type);
}

static void fb_error_check(long *isc_status)
{
	short code = isc_sqlcode(isc_status);

	if (code != 0) {
		char buf[1024];
		VALUE exc, msg, msg1, msg2;

		isc_sql_interprete(code, buf, 1024);
		msg1 = rb_str_new2(buf);
		msg2 = fb_error_msg(isc_status);
		msg = rb_str_cat(msg1, "\n", strlen("\n"));
		msg = rb_str_concat(msg, msg2);

		exc = rb_exc_new3(rb_eFbError, msg);
		rb_iv_set(exc, "error_code", INT2FIX(code));
		rb_exc_raise(exc);
	}
}

static void fb_error_check_warn(long *isc_status)
{
	short code = isc_sqlcode(isc_status);
	if (code != 0) {
		char buf[1024];
		isc_sql_interprete(code, buf, 1024);
		rb_warning("%s(%d)", buf, code);
	}
}

static XSQLDA* sqlda_alloc(long cols)
{
	XSQLDA *sqlda;

	sqlda = (XSQLDA*)xmalloc(XSQLDA_LENGTH(cols));
#ifdef SQLDA_CURRENT_VERSION
	sqlda->version = SQLDA_CURRENT_VERSION;
#else
	sqlda->version = SQLDA_VERSION1;
#endif
	sqlda->sqln = cols;
	sqlda->sqld = cols;
	return sqlda;
}

static VALUE cursor_close _((VALUE));
static VALUE cursor_drop _((VALUE));
static VALUE cursor_execute _((int, VALUE*, VALUE));

static void fb_cursor_mark();
static void fb_cursor_free();

/* connection utilities */
static void fb_connection_check(struct FbConnection *fb_connection)
{
	if (fb_connection->db == 0) {
		rb_raise(rb_eFbError, "closed db connection");
	}
}

static void fb_connection_close_cursors()
{
	struct FbConnection *list = fb_connection_list;
	int i;

	while (list) {
		for (i = 0; i < RARRAY(list->cursor)->len; i++) {
			cursor_close(RARRAY(list->cursor)->ptr[i]);
		}
		list = list->next;
	}
}

static void fb_connection_drop_cursors(struct FbConnection *fb_connection)
{
	int i;

	for (i = 0; i < RARRAY(fb_connection->cursor)->len; i++) {
		cursor_drop(RARRAY(fb_connection->cursor)->ptr[i]);
	}
	RARRAY(fb_connection->cursor)->len = 0;
}

static void fb_connection_remove(struct FbConnection *fb_connection)
{
	if (fb_connection_list != NULL) {
		if (fb_connection_list == fb_connection) {
			fb_connection_list = fb_connection_list->next;
		} else {
			struct FbConnection *list = fb_connection_list;
			while (list->next) {
				if (list->next == fb_connection) {
					list->next = fb_connection->next;
					break;
				}
				list = list->next;
			}
		}
		fb_connection->db = 0;
		db_num--;
	}
}

static void fb_connection_disconnect(struct FbConnection *fb_connection)
{
	long isc_status[20];
	
	if (transact) {
		isc_commit_transaction(isc_status, &transact);
		fb_error_check(isc_status);
	}
	if (fb_connection->dropped) {
		isc_drop_database(isc_status, &fb_connection->db);
	} else {
		isc_detach_database(isc_status, &fb_connection->db);
	}
	fb_error_check(isc_status);
	fb_connection_remove(fb_connection);
}

static void fb_connection_disconnect_warn(struct FbConnection *fb_connection)
{
	long isc_status[20];
	
	if (transact) {
		isc_commit_transaction(isc_status, &transact);
		fb_error_check_warn(isc_status);
	}
	isc_detach_database(isc_status, &fb_connection->db);
	fb_error_check_warn(isc_status);
	fb_connection_remove(fb_connection);
}

static void fb_connection_mark(struct FbConnection *fb_connection)
{
	rb_gc_mark(fb_connection->cursor);
}

static void fb_connection_free(struct FbConnection *fb_connection)
{
	if (fb_connection->db) {
		fb_connection_disconnect_warn(fb_connection);
	}
	free(fb_connection);
}

static struct FbConnection* fb_connection_check_retrieve(VALUE data)
{
	if (TYPE(data) != T_DATA || RDATA(data)->dfree != (void *)fb_connection_free) {
		rb_raise(rb_eTypeError,
			"Wrong argument type %s (expected Fb::Connection)",
			rb_class2name(CLASS_OF(data)));
	}
	return (struct FbConnection*)RDATA(data)->data;
}

static unsigned short fb_connection_db_SQL_Dialect(struct FbConnection *fb_connection)
{
	long isc_status[20];
	long dialect;
	long length;
	char db_info_command = isc_info_db_SQL_dialect;

	/* Get the db SQL Dialect */
	isc_database_info(isc_status, &fb_connection->db,
			1, &db_info_command,
			sizeof(isc_info_buff), isc_info_buff);
	fb_error_check(isc_status);

	if (isc_info_buff[0] == isc_info_db_SQL_dialect) {
		length = isc_vax_integer(&isc_info_buff[1], 2);
		dialect = isc_vax_integer(&isc_info_buff[3], (short)length);
	} else {
		dialect = 1;
	}
	return dialect;
}

static unsigned short fb_connection_dialect(struct FbConnection *fb_connection)
{
	return fb_connection->dialect;
}

static unsigned short fb_connection_db_dialect(struct FbConnection *fb_connection)
{
	return fb_connection->db_dialect;
}

/* Transaction option list */

static trans_opts	rcom_opt_S[] =
{
	"NO",			"RECORD_VERSION",	isc_tpb_no_rec_version,	-1,	0,
	"RECORD_VERSION",	0,			isc_tpb_rec_version,	-1,	0,
	"*",			0,			isc_tpb_no_rec_version,	-1,	0,
	0,			0,			0,			0,	0
};


static trans_opts	read_opt_S[] =
{
	"WRITE",	0,	isc_tpb_write,		1,	0,
	"ONLY",		0,	isc_tpb_read,		1,	0,
	"COMMITTED",	0,	isc_tpb_read_committed,	2,	rcom_opt_S,
	0,		0,	0,			0,	0
};


static trans_opts	snap_opt_S[] =
{
	"TABLE",	"STABILITY",	isc_tpb_consistency,	2,	0,
	"*",		0,		isc_tpb_concurrency,	2,	0,
	0,			0,	0,			0,	0
};


static trans_opts	isol_opt_S[] =
{
	"SNAPSHOT",	0,		0,			0,	snap_opt_S,
	"READ",		"COMMITTED",	isc_tpb_read_committed,	2,	rcom_opt_S,
	0,		0,		0,			0,	0
};


static trans_opts	trans_opt_S[] =
{
	"READ",		0,		0,		0,	read_opt_S,
	"WAIT",		0,		isc_tpb_wait,	3,	0,
	"NO",		"WAIT",		isc_tpb_nowait,	3,	0,
	"ISOLATION",	"LEVEL",	0,		0,	isol_opt_S,
	"SNAPSHOT",	0,		0,		0,	snap_opt_S,
	"RESERVING",	0,		-1,		0,	0,
	0,		0,		0,		0,	0
};

/* Name1	Name2		Option value	    Position	Sub-option */

#define	RESV_TABLEEND	"FOR"
#define	RESV_SHARED	"SHARED"
#define	RESV_PROTECTD	"PROTECTED"
#define	RESV_READ	"READ"
#define	RESV_WRITE	"WRITE"
#define	RESV_CONTINUE	','

static char* trans_parseopts(VALUE opt, int *tpb_len)
{
	char *s, *trans;
	long used;
	long size;
	char *tpb;
	trans_opts *curr_p;
	trans_opts *target_p;
	char *check1_p;
	char *check2_p;
	int count;
	int next_c;
	char check_f[4];
	char *resv_p;
	char *resend_p;
	char *tblend_p;
	int tbl_len;
	int res_first;
	int res_count;
	int ofs;
	char sp_prm;
	char rw_prm;
	int cont_f;
	char *desc = 0;

	/* Initialize */
	s = STR2CSTR(opt);
	trans = ALLOCA_N(char, strlen(s)+1);
	strcpy(trans, s);
	s = trans;
	while (*s) {
		*s = UPPER(*s);
		s++;
	}

	used = 0;
	size = 0;
	tpb = 0;
	memset((void *)check_f, 0, sizeof(check_f));

	/* Set the default transaction option */
	tpb = (char*)xmalloc(TPBBUFF_ALLOC);
	size = TPBBUFF_ALLOC;
	memcpy((void*)tpb, (void*)isc_tpb_0, sizeof(isc_tpb_0));
	used = sizeof(isc_tpb_0);

	/* Analize the transaction option strings */
	curr_p = trans_opt_S;
	check1_p = strtok(trans, CMND_DELIMIT);
	if (check1_p) {
		check2_p = strtok(0, CMND_DELIMIT);
	} else {
		check2_p = 0;
	}
	while (curr_p) {
		target_p = 0;
		next_c = 0;
		for (count = 0; curr_p[count].option1; count++) {
			if (!strcmp(curr_p[count].option1, "*")) {
				target_p = &curr_p[count];
				break;
			} else if (check1_p && !strcmp(check1_p, curr_p[count].option1)) {
				if (!curr_p[count].option2) {
					next_c = 1;
					target_p = &curr_p[count];
					break;
				} else if (check2_p && !strcmp(check2_p, curr_p[count].option2)) {
					next_c = 2;
					target_p = &curr_p[count];
					break;
				}
			}
		}

		if (!target_p) {
			desc = "Illegal transaction option was specified";
			goto error;
		}

		/* Set the transaction option */
		if (target_p->optval > '\0') {
			if (target_p->position > 0) {
				if (check_f[target_p->position]) {
					desc = "Duplicate transaction option was specified";
					goto error;
				}
				tpb[target_p->position] = target_p->optval;
				check_f[target_p->position] = 1;
			} else {
				if (used + 1 > size) {
					tpb = (char *)realloc(tpb, size + TPBBUFF_ALLOC);
					size += TPBBUFF_ALLOC;
				}
				tpb[used] = target_p->optval;
				used++;
			}
		} else if (target_p->optval) {		/* RESERVING ... FOR */
			if (check_f[0]) {
				desc = "Duplicate transaction option was specified";
				goto error;
			}
			resv_p = check2_p;
			if (!resv_p || !strcmp(resv_p, RESV_TABLEEND)) {
				desc = "RESERVING needs table name list";
				goto error;
			}
			while (resv_p) {
				res_first = used;
				res_count = 0;
				resend_p = strtok(0, CMND_DELIMIT);
				while (resend_p) {
					if (!strcmp(resend_p, RESV_TABLEEND)) {
						break;
					}
					resend_p = strtok(0, CMND_DELIMIT);
				}

				if (!resend_p) {
					desc = "Illegal transaction option was specified";
					goto error;
				}

				while (resv_p < resend_p) {
					if (*resv_p == '\0' || (ofs = strspn(resv_p, LIST_DELIMIT)) < 0) {
						resv_p++;
					} else {
						resv_p = &resv_p[ofs];
						tblend_p = strpbrk(resv_p, LIST_DELIMIT);
						if (tblend_p) {
							tbl_len = tblend_p - resv_p;
						} else {
							tbl_len = strlen(resv_p);
						}
						if (tbl_len > META_NAME_MAX) {
							desc = "Illegal table name was specified";
							goto error;
						}

						if (tbl_len > 0) {
							if (used + tbl_len + 3 > size) {
								tpb = (char*)xrealloc(tpb, size+TPBBUFF_ALLOC);
								size += TPBBUFF_ALLOC;
							}
							tpb[used+1] = (char)tbl_len;
							memcpy((void *)&tpb[used+2],resv_p, tbl_len);
							used += tbl_len + 3;
							res_count++;
						}
						resv_p += tbl_len;
					}
				}

				resv_p = strtok(0, CMND_DELIMIT);
				if (resv_p && !strcmp(resv_p, RESV_SHARED)) {
					sp_prm = isc_tpb_shared;
				} else if (resv_p && !strcmp(resv_p, RESV_PROTECTD)) {
					sp_prm = isc_tpb_protected;
				} else {
					desc = "RESERVING needs {SHARED|PROTECTED} {READ|WRITE}";
					goto error;
				}

				cont_f = 0;
				resv_p = strtok(0, CMND_DELIMIT);
				if (resv_p) {
					if (resv_p[strlen(resv_p)-1] == RESV_CONTINUE) {
						cont_f = 1;
						resv_p[strlen(resv_p)-1] = '\0';
					} else {
						tblend_p = strpbrk(resv_p, LIST_DELIMIT);
						if (tblend_p) {
							cont_f = 2;
							*tblend_p = '\0';
						}
					}
				}

				if (resv_p && !strcmp(resv_p, RESV_READ)) {
					rw_prm = isc_tpb_lock_read;
				} else if (resv_p && !strcmp(resv_p, RESV_WRITE)) {
					rw_prm = isc_tpb_lock_write;
				} else {
					desc = "RESERVING needs {SHARED|PROTECTED} {READ|WRITE}";
					goto error;
				}

				ofs = res_first;
				for (count = 0; count < res_count; count++) {
					tpb[ofs++] = rw_prm;
					ofs += tpb[ofs] + 1;
					tpb[ofs++] = sp_prm;
				}

				if (cont_f == 1) {
					resv_p = strtok(0, CMND_DELIMIT);
					if (!resv_p) {
						desc = "Unexpected end of command";
						goto error;
					}
				}
				if (cont_f == 2) {
					resv_p = tblend_p + 1;
				} else {
					resv_p = strtok(0, CMND_DELIMIT);
					if (resv_p) {
						if ((int)strlen(resv_p) == 1 && resv_p[0] == RESV_CONTINUE) {
							resv_p = strtok(0, CMND_DELIMIT);
							if (!resv_p) {
								desc = "Unexpected end of command";
								goto error;
							}
						} else if (resv_p[0] == RESV_CONTINUE) {
							resv_p++;
						} else {
							next_c = 1;
							check2_p = resv_p;
							resv_p = 0;
						}
					} else {
						next_c = 0;
						check1_p = check2_p = 0;
					}
				}
			}

			check_f[0] = 1;
		}


		/* Set the next check list */
		curr_p = target_p->sub_opts;

		for (count = 0; count < next_c; count++) {
			check1_p = check2_p;
			if (check2_p) {
				check2_p = strtok(0, CMND_DELIMIT);
			}
		}

		if (check1_p && !curr_p) {
			curr_p = trans_opt_S;
		}
	}

	/* Set the results */
	*tpb_len = used;
	return tpb;

error:
	free(tpb);
	rb_raise(rb_eFbError, desc);
}

static void set_teb_vec(ISC_TEB *vec, struct FbConnection *fb_connection, char *tpb, int len)
{
	vec->dbb_ptr = &fb_connection->db;
	if (tpb) {
		vec->tpb_ptr = tpb;
		vec->tpb_len = len;
	} else {
		vec->tpb_ptr = 0;
		vec->tpb_len = 0;
	}
}

static void transaction_start(VALUE opt, int argc, VALUE *argv)
{
	long isc_status[20];
	struct FbConnection *fb_connection;
	ISC_TEB *teb_vec = ALLOCA_N(ISC_TEB, db_num);
	ISC_TEB *vec = teb_vec;
	char *tpb = 0;
	short n;
	int tpb_len;

	if (transact) {
		rb_raise(rb_eFbError, "The transaction has been already started");
	}

	if (!NIL_P(opt)) {
		tpb = trans_parseopts(opt, &tpb_len);
	}

	if (argc > db_num) {
		rb_raise(rb_eFbError, "Too many databases specified for the transaction");
	}
	if (argc == 0) {
		n = db_num;
		for (fb_connection = fb_connection_list; fb_connection; fb_connection = fb_connection->next) {
			set_teb_vec(vec, fb_connection, tpb, tpb_len);
			vec++;
		}
	} else {
		for (n = 0; n < argc; n++) {
			fb_connection = fb_connection_check_retrieve(argv[n]);
			set_teb_vec(vec, fb_connection, tpb, tpb_len);
			vec++;
		}
	}

	isc_start_multiple(isc_status, &transact, n, teb_vec);
	if (tpb) free(tpb);
	fb_error_check(isc_status);
}

/* call-seq:
 *   transaction(options, *connections) -> nil
 *
 * Start a (global) transaction.
 */
static VALUE global_transaction(int argc, VALUE *argv, VALUE self)
{
	VALUE opt = Qnil;

	if (argc > 0) {
		opt = *argv++;
		argc--;
	}
	transaction_start(opt, argc, argv);

	return Qnil;
}

/* call-seq:
 *   transaction_started()? -> true or false
 *
 * Returns true if a transaction is currently active.
 */
static VALUE global_transaction_started()
{
	return transact ? Qtrue : Qfalse;
}

/* call-seq:
 *   commit() -> nil
 *
 * Commit the current transaction.
 */
static VALUE global_commit()
{
	long isc_status[20];
	fb_connection_close_cursors();
	if (transact) {
		isc_commit_transaction(isc_status, &transact);
		fb_error_check(isc_status);
		transact = 0;
	}
	return Qnil;
}

/* call-seq:
 *   rollback the current transaction.
 */
static VALUE global_rollback()
{
	long isc_status[20];
	fb_connection_close_cursors();
	if (transact) {
		isc_rollback_transaction(isc_status, &transact);
		fb_error_check(isc_status);
		transact = 0;
	}
	return Qnil;
}

/*
 * call-seq:
 *   open? -> true or false
 *
 * Current connection status.
 */

static VALUE connection_is_open(VALUE self)
{
	struct FbConnection *fb_connection;

	Data_Get_Struct(self, struct FbConnection, fb_connection);
	return (fb_connection->db == 0) ? Qfalse : Qtrue;
}

/* call-seq:
 *   to_s
 *
 * Return current database connection string and either (OPEN) or (CLOSED).
 */
static VALUE connection_to_s(VALUE self)
{
	VALUE is_open = connection_is_open(self);
	VALUE status = (is_open == Qtrue) ? rb_str_new2(" (OPEN)") : rb_str_new2(" (CLOSED)");
	VALUE database = rb_iv_get(self, "@database");
	VALUE s = rb_str_dup(database);
	return rb_str_concat(s, status);
}

static VALUE connection_cursor(VALUE self)
{
	long isc_status[20];
	VALUE c;
	struct FbConnection *fb_connection;
	struct FbCursor *fb_cursor;

	Data_Get_Struct(self, struct FbConnection, fb_connection);
	fb_connection_check(fb_connection);

	c = Data_Make_Struct(rb_cFbCursor, struct FbCursor, fb_cursor_mark, fb_cursor_free, fb_cursor);
	fb_cursor->connection = self;
	fb_cursor->fields_ary = Qnil;
	fb_cursor->fields_hash = Qnil;
	fb_cursor->open = Qfalse;
	fb_cursor->stmt = 0;
	isc_dsql_alloc_statement2(isc_status, &fb_connection->db, &fb_cursor->stmt);
	fb_error_check(isc_status);

	return c;
}

/* call-seq:
 *   execute(sql, *args) -> Cursor
 *   execute(sql, *args) {|cursor| }
 */
static VALUE connection_execute(int argc, VALUE *argv, VALUE self)
{
	VALUE cursor = connection_cursor(self);
	VALUE val = cursor_execute(argc, argv, cursor);

	if (NIL_P(val)) {
		if (rb_block_given_p()) {
			return rb_ensure(rb_yield,cursor,cursor_close,cursor);
   		} else {
			return cursor;
   		}
	}
	return Qnil;
}

/* call-seq:
 *   close() -> nil
 *
 * Close connection.
 */
static VALUE connection_close(VALUE self)
{
	struct FbConnection *fb_connection;

	Data_Get_Struct(self, struct FbConnection, fb_connection);

	if (fb_connection->dropped) return Qnil;

	fb_connection_check(fb_connection);
	fb_connection_disconnect(fb_connection);
	fb_connection_drop_cursors(fb_connection);

	return Qnil;
}

/* call-seq:
 *   drop() -> nil
 *
 * Drop connected database.
 */
static VALUE connection_drop(VALUE self)
{
	struct FbConnection *fb_connection;

	Data_Get_Struct(self, struct FbConnection, fb_connection);
	fb_connection->dropped = 1;
	fb_connection_disconnect(fb_connection);
	fb_connection_drop_cursors(fb_connection);

	return Qnil;
}

/* call-seq:
 *   dialect() -> int
 *
 * Returns dialect of connection.
 */
static VALUE connection_dialect(VALUE self)
{
	struct FbConnection *fb_connection;

	Data_Get_Struct(self, struct FbConnection, fb_connection);
	fb_connection_check(fb_connection);

	return INT2FIX(fb_connection->dialect);
}

/* call-seq:
 *   db_dialect() -> int
 *
 * Returns database dialect.
 */
static VALUE connection_db_dialect(VALUE self)
{
	struct FbConnection *fb_connection;

	Data_Get_Struct(self, struct FbConnection, fb_connection);
	fb_connection_check(fb_connection);

	return INT2FIX(fb_connection->db_dialect);
}

static void fb_cursor_check(struct FbCursor *fb_cursor)
{
	if (fb_cursor->stmt == 0) {
		rb_raise(rb_eFbError, "dropped db cursor");
	}
	if (!fb_cursor->open) {
		rb_raise(rb_eFbError, "closed db cursor");
	}
}

static void fb_cursor_drop(struct FbCursor *fb_cursor)
{
	long isc_status[20];
	if (fb_cursor->open) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check(isc_status);
	}
	isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_drop);
	fb_error_check(isc_status);
	fb_cursor->stmt = 0;
}

static void fb_cursor_drop_warn(struct FbCursor *fb_cursor)
{
	long isc_status[20];
	if (fb_cursor->open) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check_warn(isc_status);
	}
	isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_drop);
	fb_error_check_warn(isc_status);
	fb_cursor->stmt = 0;
}

static void fb_cursor_mark(struct FbCursor *fb_cursor)
{
	rb_gc_mark(fb_cursor->connection);
	rb_gc_mark(fb_cursor->fields_ary);
	rb_gc_mark(fb_cursor->fields_hash);
}

static void fb_cursor_free(struct FbCursor *fb_cursor)
{
	if (fb_cursor->stmt) {
		fb_cursor_drop_warn(fb_cursor);
	}
	free(fb_cursor);
}

struct time_object {
    struct timeval tv;
    struct tm tm;
    int gmt;
    int tm_got;
};

#define GetTimeval(obj, tobj) \
    Data_Get_Struct(obj, struct time_object, tobj)

static void fb_cursor_set_inputparams(struct FbCursor *fb_cursor, int argc, VALUE *argv)
{
	struct FbConnection *fb_connection;
	long count;
	long offset;
	long type;
	short dtp;
	VALUE obj;
	long lvalue;
	long alignment;
	double dvalue;
	double dcheck;
	VARY *vary;
	XSQLVAR *var;

	isc_blob_handle blob_handle;
	ISC_QUAD blob_id;
	static char blob_items[] = { isc_info_blob_max_segment };
	char blob_info[16];
	char *p;
	long length;
	struct time_object *tobj;

	long isc_status[20];
	
	Data_Get_Struct(fb_cursor->connection, struct FbConnection, fb_connection);

	/* Check the number of parameters */
	if (i_sqlda->sqld != argc) {
		rb_raise(rb_eFbError, "statement requires %d items; %d given",
			i_sqlda->sqld, argc);
	}

	/* Get the parameters */
	for (count = 0,offset = 0; count < argc; count++) {
		obj = argv[count];

		type = TYPE(obj);

		/* Convert the data type for InterBase */
		var = &i_sqlda->sqlvar[count];
		if (!NIL_P(obj)) {
			dtp = var->sqltype & ~1;		/* Erase null flag */
			alignment = var->sqllen;

			switch (dtp) {
				case SQL_TEXT :
					alignment = 1;
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					obj = rb_obj_as_string(obj);
					if (RSTRING(obj)->len > var->sqllen) {
						rb_raise(rb_eRangeError, "CHAR overflow: %d bytes exceeds %d byte(s) allowed.",
							RSTRING(obj)->len, var->sqllen);
					}
					memcpy(var->sqldata, RSTRING(obj)->ptr, RSTRING(obj)->len);
					var->sqllen = RSTRING(obj)->len;
					offset += var->sqllen + 1;
					break;

				case SQL_VARYING :
					alignment = sizeof(short);
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					vary = (VARY *)var->sqldata;
					obj = rb_obj_as_string(obj);
					if (RSTRING(obj)->len > var->sqllen) {
						rb_raise(rb_eRangeError, "VARCHAR overflow: %d bytes exceeds %d byte(s) allowed.",
							RSTRING(obj)->len, var->sqllen);
					}
					memcpy(vary->vary_string, RSTRING(obj)->ptr, RSTRING(obj)->len);
					vary->vary_length = RSTRING(obj)->len;
					offset += vary->vary_length + sizeof(short);
					break;

				case SQL_SHORT :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					lvalue = NUM2LONG(obj);
					if (lvalue < SHRT_MIN || lvalue > SHRT_MAX) {
						rb_raise(rb_eRangeError, "short integer overflow");
					}
					*(short *)var->sqldata = lvalue;
					offset += alignment;
					break;

				case SQL_LONG :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					lvalue = NUM2LONG(obj);
					*(long *)var->sqldata = lvalue;
					offset += alignment;
					break;

				case SQL_FLOAT :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					dvalue = NUM2DBL(obj);
					if (dvalue >= 0.0) {
						dcheck = dvalue;
					} else {
						dcheck = dvalue * -1;
					}
					if (dcheck != 0.0 && (dcheck < FLT_MIN || dcheck > FLT_MAX)) {
						rb_raise(rb_eRangeError, "float overflow");
					}
					*(float *)var->sqldata = dvalue;
					offset += alignment;
					break;

				case SQL_DOUBLE :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					dvalue = NUM2DBL(obj);
					*(double *)var->sqldata = dvalue;
					offset += alignment;
					break;
#if HAVE_LONG_LONG
				case SQL_INT64 :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					*(ISC_INT64 *)var->sqldata = NUM2LL(obj);
					offset += alignment;
					break;
#endif
				case SQL_BLOB :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					obj = rb_obj_as_string(obj);

					blob_handle = NULL;
					isc_create_blob2(
						isc_status,&fb_connection->db,&transact,
						&blob_handle,&blob_id,0,NULL);
					fb_error_check(isc_status);
					length = RSTRING(obj)->len;
					p = RSTRING(obj)->ptr;
					while (length >= 4096) {
						isc_put_segment(isc_status,&blob_handle,4096,p);
						fb_error_check(isc_status);
						p += 4096;
						length -= 4096;
					}
					if (length) {
						isc_put_segment(isc_status,&blob_handle,length,p);
						fb_error_check(isc_status);
					}
					isc_close_blob(isc_status,&blob_handle);
					fb_error_check(isc_status);

					*(ISC_QUAD *)var->sqldata = blob_id;
					offset += alignment;
					break;

				case SQL_TIMESTAMP :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					GetTimeval(obj, tobj);
					isc_encode_timestamp(&tobj->tm, (ISC_TIMESTAMP *)var->sqldata);
					offset += alignment;
					break;

				case SQL_TYPE_TIME :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					GetTimeval(obj, tobj);
					isc_encode_sql_time(&tobj->tm, (ISC_TIME *)var->sqldata);
					offset += alignment;
					break;

				case SQL_TYPE_DATE :
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					GetTimeval(obj, tobj);
					isc_encode_sql_date(&tobj->tm, (ISC_DATE *)var->sqldata);
					offset += alignment;
					break;

#if 0
				case SQL_ARRAY :
					/* Not supported now
					offset = ALIGN(offset, alignment);
					var->sqldata = (char *)(paramts + offset);
					if (get_arrayvalue(self, type, obj, var))
						return(STATUS_ABNORMAL);
					offset += alignment;
					break;
					*/
					rb_raise(rb_eArgError, "Arrays not supported");
					break;
#endif

				default :
					rb_raise(rb_eFbError, "Specified table includes unsupported datatype (%d)", dtp);
			}

			if (var->sqltype & 1) {
				offset = ALIGN(offset, sizeof(short));
				var->sqlind = (short *)(paramts + offset);
				*var->sqlind = 0;
				offset += sizeof(short);
			}
		} else if (var->sqltype & 1) {
			var->sqldata = 0;
			offset = ALIGN(offset, sizeof(short));
			var->sqlind = (short *)(paramts + offset);
			*var->sqlind = -1;
			offset += sizeof(short);
		} else {
			rb_raise(rb_eFbError, "specified column is not permitted to be null");
		}
	}
}

static void fb_cursor_execute_withparams(struct FbCursor *fb_cursor, int argc, VALUE *argv)
{
	struct FbConnection *fb_connection;
	long isc_status[20];

	Data_Get_Struct(fb_cursor->connection, struct FbConnection, fb_connection);
	/* Check the first object type of the parameters */
	if (argc >= 1 && TYPE(argv[0]) == T_ARRAY) {
		VALUE obj;
		int i;

		for (i = 0; i < argc; i++) {
			obj = argv[i];

			/* Set the input parameters */
			Check_Type(obj, T_ARRAY);
			fb_cursor_set_inputparams(fb_cursor, RARRAY(obj)->len, RARRAY(obj)->ptr);

			/* Execute SQL statement */
			isc_dsql_execute2(isc_status, &transact, &fb_cursor->stmt, 1, i_sqlda, 0);
			fb_error_check(isc_status);
		}
	} else {
		/* Set the input parameters */
		fb_cursor_set_inputparams(fb_cursor, argc, argv);

		/* Execute SQL statement */
		isc_dsql_execute2(isc_status, &transact, &fb_cursor->stmt, 1, i_sqlda, 0);
		fb_error_check(isc_status);
	}
}

static VALUE precision_from_sqlvar(XSQLVAR *sqlvar)
{
	switch(sqlvar->sqltype & ~1) {
		case SQL_TEXT:		return Qnil;
		case SQL_VARYING:	return Qnil;
		case SQL_SHORT:
			switch (sqlvar->sqlsubtype) {
				case 0:		return INT2FIX(0);
				case 1:		return INT2FIX(4);
				case 2:		return INT2FIX(4);
			}
			break;
		case SQL_LONG:
			switch (sqlvar->sqlsubtype) {
				case 0:		return INT2FIX(0);
				case 1:		return INT2FIX(9);
				case 2:		return INT2FIX(9);
			}
			break;
		case SQL_FLOAT:		return Qnil;
		case SQL_DOUBLE:
		case SQL_D_FLOAT:
			switch (sqlvar->sqlsubtype) {
				case 0:		return Qnil;
				case 1:		return INT2FIX(15);
				case 2:		return INT2FIX(15);
			}
			break;
		case SQL_TIMESTAMP:	return Qnil;
		case SQL_BLOB:		return Qnil;
		case SQL_ARRAY:		return Qnil;
		case SQL_QUAD:		return Qnil;
		case SQL_TYPE_TIME:	return Qnil;
		case SQL_TYPE_DATE:	return Qnil;
		case SQL_INT64:
			switch (sqlvar->sqlsubtype) {
				case 0:		return INT2FIX(0);
				case 1:		return INT2FIX(18);
				case 2:		return INT2FIX(18);
			}
			break;
	}
	return Qnil;
}

static VALUE fb_cursor_fields_ary(XSQLDA *sqlda)
{
	long cols;
	long count;
	XSQLVAR *var;
	short dtp;
	VALUE ary;

	cols = sqlda->sqld;
	if (cols == 0) {
		return Qnil;
	}

	ary = rb_ary_new();
	for (count = 0; count < cols; count++) {
		VALUE field;
		VALUE name, type_code, sql_type, sql_subtype, display_size, internal_size, precision, scale, nullable;

		var = &sqlda->sqlvar[count];
		dtp = var->sqltype & ~1;

		name = rb_tainted_str_new(var->sqlname, var->sqlname_length);
		rb_str_freeze(name);
		type_code = INT2NUM((long)(var->sqltype & ~1));
		sql_type = fb_sql_type_from_code(dtp, var->sqlsubtype);
		sql_subtype = INT2FIX(var->sqlsubtype);
		display_size = INT2NUM((long)var->sqllen);
		if (dtp == SQL_VARYING) {
			internal_size = INT2NUM((long)var->sqllen + sizeof(short));
		} else {
			internal_size = INT2NUM((long)var->sqllen);
		}
		precision = precision_from_sqlvar(var);
		scale = INT2NUM((long)var->sqlscale);
		nullable = (var->sqltype & 1) ? Qtrue : Qfalse;

		field = rb_struct_new(rb_sFbField, name, sql_type, sql_subtype, display_size, internal_size, precision, scale, nullable, type_code);
		rb_ary_push(ary, field);
	}
	rb_ary_freeze(ary);
	return ary;
}

static VALUE fb_cursor_fields_hash(VALUE fields_ary)
{
	int i;
	VALUE hash = rb_hash_new();

	for (i = 0; i < RARRAY(fields_ary)->len; i++) {
		VALUE field = rb_ary_entry(fields_ary, i);
		VALUE name = rb_struct_aref(field, LONG2NUM(0));
		rb_hash_aset(hash, name, field);
	}

	return hash;
}

/* Check the input parameters */
static void fb_cursor_check_inparams(struct FbCursor *fb_cursor, int argc, VALUE *argv, int exec)
{
	long items;

	/* Check the parameter */
	if (argc == 0) {
		rb_raise(rb_eFbError, "Input parameters must be specified");
	}

	/* Execute specified process */
	switch (exec) {
		case EXECF_EXECDML:
			fb_cursor_execute_withparams(fb_cursor, argc, argv);
			break;
		case EXECF_SETPARM:
			fb_cursor_set_inputparams(fb_cursor, argc, argv);
			break;
		default:
			rb_raise(rb_eFbError, "Should specify either EXECF_EXECDML or EXECF_SETPARM");
			break;
	}
}

static void fb_cursor_fetch_prep(struct FbCursor *fb_cursor)
{
	struct FbConnection *fb_connection;
	long cols;
	long count;
	XSQLVAR *var;
	short dtp;
	long length;
	long alignment;
	long offset;
	long isc_status[20];

	fb_cursor_check(fb_cursor);

	Data_Get_Struct(fb_cursor->connection, struct FbConnection, fb_connection);
	fb_connection_check(fb_connection);

	/* Check if open cursor */
	if (!fb_cursor->open) {
		rb_raise(rb_eFbError, "The cursor has not been open. Use execute(query)");
	}
	/* Describe output SQLDA */
	isc_dsql_describe(isc_status, &fb_cursor->stmt, 1, o_sqlda);
	fb_error_check(isc_status);

	/* Set the output SQLDA */
	cols = o_sqlda->sqld;
	for (var = o_sqlda->sqlvar, offset = 0, count = 0; count < cols; var++, count++) {
		length = alignment = var->sqllen;
		dtp = var->sqltype & ~1;

		if (dtp == SQL_TEXT) {
			alignment = 1;
		} else if (dtp == SQL_VARYING) {
			length += sizeof(short);
			alignment = sizeof(short);
		}
		offset = ALIGN(offset, alignment);
		var->sqldata = (char*)(results + offset);
		offset += length;
		offset = ALIGN(offset, sizeof(short));
		var->sqlind = (short*)(results + offset);
		offset += sizeof(short);
	}
}

static VALUE fb_cursor_fetch(struct FbCursor *fb_cursor)
{
	struct FbConnection *fb_connection;
	long cols;
	VALUE ary;
	long count;
	XSQLVAR *var;
	long dtp;
	VALUE val;
	VARY *vary;
	double ratio;
	double dval;
	long scnt;
	struct tm tms;

	isc_blob_handle blob_handle;
	ISC_QUAD blob_id;
	unsigned short actual_seg_len;
	time_t t;
	static char blob_items[] = {
		isc_info_blob_max_segment,
		isc_info_blob_num_segments,
		isc_info_blob_total_length
	};
	char blob_info[32];
	char *p, item;
	short length;
	unsigned short max_segment;
	ISC_LONG num_segments;
	ISC_LONG total_length;

	long isc_status[20];
	
	Data_Get_Struct(fb_cursor->connection, struct FbConnection, fb_connection);
	fb_connection_check(fb_connection);

	/* Fetch one row */
	if (isc_dsql_fetch(isc_status, &fb_cursor->stmt, 1, o_sqlda) == SQLCODE_NOMORE) {
		return Qnil;
	}
	fb_error_check(isc_status);

	/* Create the result tuple object */
	cols = o_sqlda->sqld;
	ary = rb_ary_new2(cols);

	/* Create the result objects for each columns */
	for (count = 0; count < cols; count++) {
		var = &o_sqlda->sqlvar[count];
		dtp = var->sqltype & ~1;

		/* Check if column is null */

		if ((var->sqltype & 1) && (*var->sqlind < 0)) {
			val = Qnil;
		} else {
			/* Set the column value to the result tuple */

			switch (dtp) {
				case SQL_TEXT:
					val = rb_tainted_str_new(var->sqldata, var->sqllen);
					break;

				case SQL_VARYING:
					vary = (VARY*)var->sqldata;
					val = rb_tainted_str_new(vary->vary_string, vary->vary_length);
					break;

				case SQL_SHORT:
					if (var->sqlscale < 0) {
						ratio = 1;
						for (scnt = 0; scnt > var->sqlscale; scnt--)
							ratio *= 10;
						dval = (double)*(short*)var->sqldata/ratio;
						val = rb_float_new(dval);
					} else {
						val = INT2NUM((long)*(short*)var->sqldata);
					}
					break;

				case SQL_LONG:
					if (var->sqlscale < 0) {
						ratio = 1;
						for (scnt = 0; scnt > var->sqlscale; scnt--)
						ratio *= 10;
						dval = (double)*(long*)var->sqldata/ratio;
						val = rb_float_new(dval);
					} else {
						val = INT2NUM(*(long*)var->sqldata);
					}
					break;

				case SQL_FLOAT:
					val = rb_float_new((double)*(float*)var->sqldata);
					break;

				case SQL_DOUBLE:
					val = rb_float_new(*(double*)var->sqldata);
					break;
#if HAVE_LONG_LONG
				case SQL_INT64:
					val = LL2NUM(*(LONG_LONG*)var->sqldata);
					break;
#endif
				case SQL_TIMESTAMP:
					isc_decode_timestamp((ISC_TIMESTAMP *)var->sqldata, &tms);
					t = mktime(&tms);
					if (t < 0) t = 0;
					val = rb_time_new(t, 0);
					break;

				case SQL_TYPE_TIME:
					isc_decode_sql_time((ISC_TIME *)var->sqldata, &tms);
					/*
					t = mktime(&tms);
					if (t < 0) t = t + (24 * 60 * 60);
					val = rb_time_new(t, 0);
					*/
					tms.tm_year = 1970;
					tms.tm_mon = 1;
					tms.tm_mday = 1;
					val = fb_mktime(&tms);
					break;

				case SQL_TYPE_DATE:
					isc_decode_sql_date((ISC_DATE *)var->sqldata, &tms);
					t = mktime(&tms);
					if (t < 0) t = 0;
					val = rb_time_new(t, 0);
					break;

				case SQL_BLOB:
					blob_handle = NULL;
					blob_id = *(ISC_QUAD *)var->sqldata;
					isc_open_blob2(isc_status, &fb_connection->db, &transact, &blob_handle, &blob_id, 0, NULL);
					fb_error_check(isc_status);
					isc_blob_info(
						isc_status, &blob_handle,
						sizeof(blob_items), blob_items,
						sizeof(blob_info), blob_info);
					fb_error_check(isc_status);
					for (p = blob_info; *p != isc_info_end; p += length) {
						item = *p++;
						length = (short) isc_vax_integer(p,2);
						p += 2;
						switch (item) {
							case isc_info_blob_max_segment:
								max_segment = isc_vax_integer(p,length);
								break;
							case isc_info_blob_num_segments:
								num_segments = isc_vax_integer(p,length);
								break;
							case isc_info_blob_total_length:
								total_length = isc_vax_integer(p,length);
								break;
						}
					}
					val = rb_tainted_str_new(NULL,total_length);
					for (p = RSTRING(val)->ptr; num_segments > 0; num_segments--, p += actual_seg_len) {
						isc_get_segment(isc_status, &blob_handle, &actual_seg_len, max_segment, p);
						fb_error_check(isc_status);
					}
					isc_close_blob(isc_status, &blob_handle);
					fb_error_check(isc_status);
					break;

				case SQL_ARRAY:
					rb_warn("ARRAY not supported (yet)");
					val = Qnil;
					break;

				default:
					rb_raise(rb_eFbError, "Specified table includes unsupported datatype (%d)", dtp);
					break;
			}
		}
		rb_ary_push(ary, val);
	}

	return ary;
}

/* call-seq:
 *   execute(sql, *args)
 */
static VALUE cursor_execute(int argc, VALUE* argv, VALUE self)
{
	struct FbCursor *fb_cursor;
	struct FbConnection *fb_connection;
	char *sql;
	long statement;
	long length;
	long in_params;
	long cols;
	long items;

	long isc_status[20];
	
	Data_Get_Struct(self, struct FbCursor, fb_cursor);

	Data_Get_Struct(fb_cursor->connection, struct FbConnection, fb_connection);
	fb_connection_check(fb_connection);

	if (argc < 1) {
		rb_raise(rb_eArgError, "too few arguments (at least 1)");
	}
	sql = STR2CSTR(*argv);
	argc--; argv++;

	if (fb_cursor->open) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check(isc_status);
		fb_cursor->open = Qfalse;
	}
	if (!transact) {
		transaction_start(Qnil, 0, 0);
	}

	/* Prepare query */
	isc_dsql_prepare(isc_status, &transact, &fb_cursor->stmt, 0, sql, fb_connection_dialect(fb_connection), o_sqlda);
	fb_error_check(isc_status);

	/* Get the statement type */
	isc_dsql_sql_info(isc_status, &fb_cursor->stmt,
			sizeof(isc_info_stmt), isc_info_stmt,
			sizeof(isc_info_buff), isc_info_buff);
	fb_error_check(isc_status);

	if (isc_info_buff[0] == isc_info_sql_stmt_type) {
		length = isc_vax_integer(&isc_info_buff[1], 2);
		statement = isc_vax_integer(&isc_info_buff[3], (short)length);
	} else {
		statement = 0;
	}
	/* Describe the parameters */
	isc_dsql_describe_bind(isc_status, &fb_cursor->stmt, 1, i_sqlda);
	fb_error_check(isc_status);

	isc_dsql_describe(isc_status, &fb_cursor->stmt, 1, o_sqlda);
	fb_error_check(isc_status);

	/* Get the number of parameters and reallocate the SQLDA */
	in_params = i_sqlda->sqld;
	if (i_sqlda->sqln < in_params) {
		free(i_sqlda);
		i_sqlda = sqlda_alloc(in_params);
		/* Describe again */
		isc_dsql_describe_bind(isc_status, &fb_cursor->stmt, 1, i_sqlda);
		fb_error_check(isc_status);
	}

    /* Get the size of parameters buffer and reallocate it */
	if (in_params) {
		length = calculate_buffsize(i_sqlda);
		if (length > prmsize) {
			paramts = xrealloc(paramts, length);
			prmsize = length;
		}
	}

    /* Execute the SQL statement if it is not query */
	if (!o_sqlda->sqld) {
		if (statement == isc_info_sql_stmt_start_trans) {
			rb_raise(rb_eFbError, "use Fb::Connection#transaction()");
		} else if (statement == isc_info_sql_stmt_commit) {
			rb_raise(rb_eFbError, "use Fb::Connection#commit()");
		} else if (statement == isc_info_sql_stmt_rollback) {
			rb_raise(rb_eFbError, "use Fb::Connection#rollback()");
		} else if (in_params) {
			fb_cursor_check_inparams(fb_cursor, argc, argv, EXECF_EXECDML);
		} else {
			isc_dsql_execute2(isc_status, &transact, &fb_cursor->stmt, 1, 0, 0);
			fb_error_check(isc_status);
		}
	} else {
		/* Open cursor if the SQL statement is query */
		/* Get the number of columns and reallocate the SQLDA */
		cols = o_sqlda->sqld;
		if (o_sqlda->sqln < cols) {
			free(o_sqlda);
			o_sqlda = sqlda_alloc(cols);
			/* Describe again */
			isc_dsql_describe(isc_status, &fb_cursor->stmt, 1, o_sqlda);
			fb_error_check(isc_status);
		}

		if (in_params) {
			fb_cursor_check_inparams(fb_cursor, argc, argv, EXECF_SETPARM);
		}

		/* Open cursor */
		isc_dsql_execute2(isc_status, &transact,
				&fb_cursor->stmt, 1,
				in_params ? i_sqlda : 0, 0);
		fb_error_check(isc_status);
		fb_cursor->open = Qtrue;

		/* Get the size of results buffer and reallocate it */
		length = calculate_buffsize(o_sqlda);
		if (length > ressize) {
			results = xrealloc(results, length);
			ressize = length;
		}

		/* Set the description attributes */
		fb_cursor->fields_ary = fb_cursor_fields_ary(o_sqlda);
		fb_cursor->fields_hash = fb_cursor_fields_hash(fb_cursor->fields_ary);
	}
	/* Set the return object */
	if (statement == isc_info_sql_stmt_select ||
		statement == isc_info_sql_stmt_select_for_upd) {
		return Qnil;
	} else if (statement == isc_info_sql_stmt_ddl) {
		return INT2NUM(STATEMENT_DDL);
	}
	return INT2NUM(STATEMENT_DML);
}

static VALUE fb_hash_from_ary(VALUE fields, VALUE row)
{
	VALUE hash = rb_hash_new();
	int i;
	for (i = 0; i < RARRAY(fields)->len; i++) {
		VALUE field = rb_ary_entry(fields, i);
		VALUE name = rb_struct_aref(field, LONG2NUM(0));
		VALUE v = rb_ary_entry(row, i);
		rb_hash_aset(hash, name, v);
	}
	return hash;
}

static int hash_format(int argc, VALUE *argv)
{
	if (argc == 0 || argv[0] == ID2SYM(rb_intern("array"))) {
		return 0;
	} else if (argv[0] == ID2SYM(rb_intern("hash"))) {
		return 1;
	} else {
		rb_raise(rb_eFbError, "Unknown format");
	}
}

static VALUE cursor_fetch(int argc, VALUE* argv, VALUE self)
{
	VALUE ary;
	struct FbCursor *fb_cursor;

	int hash_row = hash_format(argc, argv);

	Data_Get_Struct(self, struct FbCursor, fb_cursor);
	fb_cursor_fetch_prep(fb_cursor);

	ary = fb_cursor_fetch(fb_cursor);
	return hash_row ? fb_hash_from_ary(fb_cursor->fields_ary, ary) : ary;
}

static VALUE cursor_fetchall(int argc, VALUE* argv, VALUE self)
{
	VALUE ary, row;
	struct FbCursor *fb_cursor;

	int hash_rows = hash_format(argc, argv);

	Data_Get_Struct(self, struct FbCursor, fb_cursor);
	fb_cursor_fetch_prep(fb_cursor);

	ary = rb_ary_new();
	for (;;) {
		row = fb_cursor_fetch(fb_cursor);
		if (NIL_P(row)) break;
		if (hash_rows) {
			rb_ary_push(ary, fb_hash_from_ary(fb_cursor->fields_ary, row));
		} else {
			rb_ary_push(ary, row);
		}
	}

	return ary;
}

static VALUE cursor_each(int argc, VALUE* argv, VALUE self)
{
	VALUE ary, row;
	struct FbCursor *fb_cursor;

	int hash_rows = hash_format(argc, argv);

	Data_Get_Struct(self, struct FbCursor, fb_cursor);
	fb_cursor_fetch_prep(fb_cursor);

	for (;;) {
		row = fb_cursor_fetch(fb_cursor);
		if (NIL_P(row)) break;
		if (hash_rows) {
			rb_yield(fb_hash_from_ary(fb_cursor->fields_ary, row));
		} else {
			rb_yield(row);
		}
	}

	return Qnil;
}

static VALUE cursor_close(VALUE self)
{
	struct FbCursor *fb_cursor;
	long isc_status[20];

	Data_Get_Struct(self, struct FbCursor, fb_cursor);
	fb_cursor_check(fb_cursor);

	/* Close the cursor */
	if (fb_cursor->stmt) {
		isc_dsql_free_statement(isc_status, &fb_cursor->stmt, DSQL_close);
		fb_error_check(isc_status);
		fb_cursor->open = Qfalse;
	}
	fb_cursor->fields_ary = Qnil;
	fb_cursor->fields_hash = Qnil;

	return Qnil;
}

/* call-seq:
 *   drop() -> nil
 *
 * Drop the cursor.
 *
 * TODO: How is this different from close()?
 */
static VALUE cursor_drop(VALUE self)
{
	struct FbCursor *fb_cursor;
	struct FbConnection *fb_connection;
	int i;

	Data_Get_Struct(self, struct FbCursor, fb_cursor);
	fb_cursor_drop(fb_cursor);
	fb_cursor->fields_ary = Qnil;
	fb_cursor->fields_hash = Qnil;

	/* reset the reference from connection */
	Data_Get_Struct(fb_cursor->connection, struct FbConnection, fb_connection);
	for (i = 0; i < RARRAY(fb_connection->cursor)->len; i++) {
		if (RARRAY(fb_connection->cursor)->ptr[i] == self) {
			RARRAY(fb_connection->cursor)->ptr[i] = Qnil;
		}
	}

	return Qnil;
}

/* call-seq:
 *   fields() -> Array
 *   fields(:array) -> Array
 *   fields(:hash) -> Hash
 *
 * Return an array of Field Structs or a hash indexed by field name.
 */
static VALUE cursor_fields(int argc, VALUE* argv, VALUE self)
{
	struct FbCursor *fb_cursor;

	Data_Get_Struct(self, struct FbCursor, fb_cursor);
	if (argc == 0 || argv[0] == ID2SYM(rb_intern("array"))) {
		return fb_cursor->fields_ary;
	} else if (argv[0] == ID2SYM(rb_intern("hash"))) {
		return fb_cursor->fields_hash;
	} else {
		rb_raise(rb_eFbError, "Unknown format");
	}
}

static VALUE error_error_code(VALUE error)
{
	rb_p(error);
	return rb_iv_get(error, "error_code");
}

static char* dbp_create(int *length)
{
	char *dbp = ALLOC_N(char, 1);
	*dbp = isc_dpb_version1;
	*length = 1;
	return dbp;
}

static char* dbp_add_string(char *dbp, char isc_dbp_code, char *s, int *length)
{
	char *buf;
	int old_length = *length;
	int s_len = strlen(s);
	*length += 2 + s_len;
	REALLOC_N(dbp, char, *length);
	buf = dbp + old_length;
	*buf++ = isc_dbp_code;
	*buf++ = (char)s_len;
	memcpy(buf, s, s_len);
	return dbp;
}

static char* connection_create_dbp(VALUE self, int *length)
{
	char *dbp;
	VALUE username, password, charset, role;

	username = rb_iv_get(self, "@username");
	Check_Type(username, T_STRING);
	password = rb_iv_get(self, "@password");
	Check_Type(password, T_STRING);
	role = rb_iv_get(self, "@role");
	charset = rb_iv_get(self, "@charset");

	dbp = dbp_create(length);
	dbp = dbp_add_string(dbp, isc_dpb_user_name, STR2CSTR(username), length);
	dbp = dbp_add_string(dbp, isc_dpb_password, STR2CSTR(password), length);
	if (!NIL_P(charset)) {
		dbp = dbp_add_string(dbp, isc_dpb_lc_ctype, STR2CSTR(charset), length);
	}
	if (!NIL_P(role)) {
		dbp = dbp_add_string(dbp, isc_dpb_sql_role_name, STR2CSTR(role), length);
	}
	return dbp;
}

static char* CONNECTION_PARMS[6] = {
	"@database",
	"@username",
	"@password",
	"@charset",
	"@role",
	(char *)0
};

static VALUE connection_create(isc_db_handle handle, VALUE db)
{
	unsigned short dialect;
	unsigned short db_dialect;
	char *parm;
	int i;
	struct FbConnection *fb_connection;
	VALUE connection = Data_Make_Struct(rb_cFbConnection, struct FbConnection, fb_connection_mark, fb_connection_free, fb_connection);
	fb_connection->db = handle;
	transact = 0;
	i_sqlda = sqlda_alloc(SQLDA_COLSINIT);
	o_sqlda = sqlda_alloc(SQLDA_COLSINIT);
	results = paramts = 0;
	ressize = prmsize = 0;
	fb_connection->cursor = rb_ary_new();
	db_num++;
	fb_connection->next = fb_connection_list;
	fb_connection_list = fb_connection;

	dialect = SQL_DIALECT_CURRENT;
	db_dialect = fb_connection_db_SQL_Dialect(fb_connection);

	if (db_dialect < dialect) {
		dialect = db_dialect;
		/* TODO: downgrade warning */
	}

	fb_connection->dialect = dialect;
	fb_connection->db_dialect = db_dialect;

	for (i = 0; parm = CONNECTION_PARMS[i]; i++) {
		rb_iv_set(connection, parm, rb_iv_get(db, parm));
	}

	return connection;
}

static VALUE connection_names(VALUE self, char *sql)
{
	VALUE row;
	VALUE query = rb_str_new2(sql);
	VALUE cursor = connection_execute(1, &query, self);
	VALUE names = rb_ary_new();
	ID id_rstrip_bang = rb_intern("rstrip!");
	
	while ((row = cursor_fetch(0, NULL, cursor)) != Qnil) {
		VALUE name = rb_ary_entry(row, 0);
		rb_ary_push(names, rb_funcall(name, id_rstrip_bang, 0));
	}

	cursor_close(cursor);
	return names;
}

/* call-seq:
 *   table_names() -> array
 *
 * Returns sorted array of table names in connected database.
 */
static VALUE connection_table_names(VALUE self)
{
	char *sql = "SELECT RDB$RELATION_NAME FROM RDB$RELATIONS "
				"WHERE (RDB$SYSTEM_FLAG <> 1 OR RDB$SYSTEM_FLAG IS NULL) AND RDB$VIEW_BLR IS NULL "
				"ORDER BY RDB$RELATION_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   generator_names() -> array
 *
 * Returns sorted array of generator names in connected database.
 */
static VALUE connection_generator_names(VALUE self)
{
	char *sql = "SELECT RDB$GENERATOR_NAME FROM RDB$GENERATORS "
				"WHERE (RDB$SYSTEM_FLAG IS NULL OR RDB$SYSTEM_FLAG <> 1) "
				"ORDER BY RDB$GENERATOR_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   view_names() -> array
 *
 * Returns sorted array of view names in connected database.
 */
static VALUE connection_view_names(VALUE self)
{
	char *sql = "SELECT RDB$RELATION_NAME, RDB$OWNER_NAME, RDB$VIEW_SOURCE FROM RDB$RELATIONS "
				"WHERE (RDB$SYSTEM_FLAG <> 1 OR RDB$SYSTEM_FLAG IS NULL) AND NOT RDB$VIEW_BLR IS NULL AND RDB$FLAGS = 1 "
				"ORDER BY RDB$RELATION_ID";
	return connection_names(self, sql);
}

/* call-seq:
 *   role_names() -> array
 *
 * Returns sorted array of role names in connected database.
 */
static VALUE connection_role_names(VALUE self)
{
	char *sql = "SELECT * FROM RDB$ROLES ORDER BY RDB$ROLE_NAME";
	return connection_names(self, sql);
}

/* call-seq:
 *   procedure_names() -> array
 *
 * Returns sorted array of stored procedure names in connected database.
 */
static VALUE connection_procedure_names(VALUE self)
{
	char *sql = "SELECT RDB$PROCEDURE_NAME FROM RDB$PROCEDURES "
				"ORDER BY RDB$PROCEDURE_NAME";
	return connection_names(self, sql);
}

static void define_attrs(VALUE klass, char **attrs)
{
	char *parm;
	while (parm = *attrs)
	{
		rb_define_attr(klass, parm+1, 1, 1);
		attrs++;
	}
}

static VALUE default_string(VALUE hash, char *key, char *def)
{
	VALUE sym = ID2SYM(rb_intern(key));
	VALUE val = rb_hash_aref(hash, sym);
	return NIL_P(val) ? rb_str_new2(def) : StringValue(val);
}

static VALUE default_int(VALUE hash, char *key, int def)
{
	VALUE sym = ID2SYM(rb_intern(key));
	VALUE val = rb_hash_aref(hash, sym);
	return NIL_P(val) ? INT2NUM(def) : val;
}

static VALUE database_allocate_instance(VALUE klass)
{
    NEWOBJ(obj, struct RObject);
    OBJSETUP(obj, klass, T_OBJECT);
    return (VALUE)obj;
}

static VALUE hash_from_connection_string(VALUE cs)
{
	VALUE hash = rb_hash_new();
	VALUE re_SemiColon = rb_reg_regcomp(rb_str_new2("\\s*;\\s*"));
	VALUE re_Equal = rb_reg_regcomp(rb_str_new2("\\s*=\\s*"));
	ID id_split = rb_intern("split");
	VALUE pairs = rb_funcall(cs, id_split, 1, re_SemiColon);
	int i;
	for (i = 0; i < RARRAY(pairs)->len; i++) {
		VALUE pair = rb_ary_entry(pairs, i);
		VALUE keyValue = rb_funcall(pair, id_split, 1, re_Equal);
		if (RARRAY(keyValue)->len == 2) {
			VALUE key = rb_ary_entry(keyValue, 0);
			VALUE val = rb_ary_entry(keyValue, 1);
			rb_hash_aset(hash, rb_str_intern(key), val);
		}
	}
	return hash;
}

/* call-seq:
 *   Database.new(options) -> Database
 *
 * Initialize Database with Hash of values:
 *   :database: Full Firebird connection string, e.g. 'localhost:/var/fbdata/drivertest.fdb' (required)
 *   :username: database username (default: 'sysdba')
 *   :password: database password (default: 'masterkey')
 *   :charset: character set to be used with the connection (default: 'NONE')
 *   :role: database role to connect using (default: nil)
 *   :page_size: page size to use when creating a database (default: 1024)
 */
static VALUE database_initialize(int argc, VALUE *argv, VALUE self)
{
	VALUE parms, database;

	if (argc >= 1) {
		parms = argv[0];
		if (TYPE(parms) == T_STRING) {
			parms = hash_from_connection_string(parms);
		} else {
			Check_Type(parms, T_HASH);
		}
		database = rb_hash_aref(parms, ID2SYM(rb_intern("database")));
		if (NIL_P(database)) rb_raise(rb_eFbError, "Database must be specified.");
		rb_iv_set(self, "@database", database);
		rb_iv_set(self, "@username", default_string(parms, "username", "sysdba"));
		rb_iv_set(self, "@password", default_string(parms, "password", "masterkey"));
		rb_iv_set(self, "@charset", default_string(parms, "charset", "NONE"));
		rb_iv_set(self, "@role", rb_hash_aref(parms, ID2SYM(rb_intern("role"))));
		rb_iv_set(self, "@page_size", default_int(parms, "page_size", 1024));
	}
	return self;
}

/* call-seq:
 *   create() -> Database
 *   create() {|connection| } -> Database
 *
 * Create a database using the current database options.
 */
static VALUE database_create(VALUE self)
{
	long isc_status[20];
	isc_db_handle handle = 0;
	isc_tr_handle transaction = 0;
	VALUE parms, fmt, stmt;
	char *sql;

	VALUE database = rb_iv_get(self, "@database");
	VALUE username = rb_iv_get(self, "@username");
	VALUE password = rb_iv_get(self, "@password");
	VALUE page_size = rb_iv_get(self, "@page_size");
	VALUE charset = rb_iv_get(self, "@charset");
	parms = rb_ary_new3(5, database, username, password, page_size, charset);

	fmt = rb_str_new2("CREATE DATABASE '%s' USER '%s' PASSWORD '%s' PAGE_SIZE = %d DEFAULT CHARACTER SET %s;");
	stmt = rb_funcall(fmt, rb_intern("%"), 1, parms);
	sql = StringValuePtr(stmt);

	if (isc_dsql_execute_immediate(isc_status, &handle, &transaction, 0, sql, 3, NULL) != 0) {
		fb_error_check(isc_status);
	}
	if (handle) {
		if (rb_block_given_p()) {
			VALUE connection = connection_create(handle, self);
			rb_ensure(rb_yield,connection,connection_close,connection);
		} else {
			isc_detach_database(isc_status, &handle);
			fb_error_check(isc_status);
		}
	}

	return self;
}

/* call-seq:
 *   Database.create(options) -> Database
 *   Database.create(options) {|connection| } -> Database
 *
 * Create a database using the specified options (see: Database.new for details of options Hash).
 */
static VALUE database_s_create(int argc, VALUE *argv, VALUE klass)
{
	VALUE obj = database_allocate_instance(klass);
	database_initialize(argc, argv, obj);
	return database_create(obj);
}

/* call-seq:
 *   connect() -> Connection
 *   connect() {|connection| } -> nil
 *
 * Connect to the database specified by the current database options.
 */
static VALUE database_connect(VALUE self)
{
	long isc_status[20];
	char *dbp;
	int length;
	isc_db_handle handle = NULL;
	VALUE database = rb_iv_get(self, "@database");
	Check_Type(database, T_STRING);
	dbp = connection_create_dbp(self, &length);
	isc_attach_database(isc_status, 0, STR2CSTR(database), &handle, length, dbp);
	free(dbp);
	fb_error_check(isc_status);
	{
		VALUE connection = connection_create(handle, self);
		if (rb_block_given_p()) {
			return rb_ensure(rb_yield, connection, connection_close, connection);
			return Qnil;
		} else {
			return connection;
		}
	}
}

/* call-seq:
 *   Database.connect(options) -> Connection
 *   Database.connect(options) {|connection| } -> nil
 *
 * Connect to a database using the options given (see: Database.new for details of options Hash).
 */
static VALUE database_s_connect(int argc, VALUE *argv, VALUE klass)
{
	VALUE obj = database_allocate_instance(klass);
	database_initialize(argc, argv, obj);
	return database_connect(obj);
}

/* call-seq:
 *   drop() -> nil
 *
 * Drop the database specified by the current database options.
 */
static VALUE database_drop(VALUE self)
{
	long isc_status[20];
	struct FbConnection *fb_connection;

	VALUE connection = database_connect(self);
	Data_Get_Struct(connection, struct FbConnection, fb_connection);
	isc_drop_database(isc_status, &fb_connection->db);
	fb_error_check(isc_status);
	fb_connection_remove(fb_connection);
	return Qnil;
}

/* call-seq:
 *   Database.drop(options) -> nil
 *
 * Drop the database specified by the options given (see: Database.new for details of options Hash).
 */
static VALUE database_s_drop(int argc, VALUE *argv, VALUE klass)
{
	VALUE obj = database_allocate_instance(klass);
	database_initialize(argc, argv, obj);
	return database_drop(obj);
}

void Init_fb()
{
	rb_mFb = rb_define_module("Fb");

	rb_cFbDatabase = rb_define_class_under(rb_mFb, "Database", rb_cData);
    rb_define_alloc_func(rb_cFbDatabase, database_allocate_instance);
    rb_define_method(rb_cFbDatabase, "initialize", database_initialize, -1);
	rb_define_attr(rb_cFbDatabase, "database", 1, 1);
	rb_define_attr(rb_cFbDatabase, "username", 1, 1);
	rb_define_attr(rb_cFbDatabase, "password", 1, 1);
	rb_define_attr(rb_cFbDatabase, "charset", 1, 1);
	rb_define_attr(rb_cFbDatabase, "role", 1, 1);
	rb_define_attr(rb_cFbDatabase, "page_size", 1, 1);
    rb_define_method(rb_cFbDatabase, "create", database_create, 0);
	rb_define_singleton_method(rb_cFbDatabase, "create", database_s_create, -1);
	rb_define_method(rb_cFbDatabase, "connect", database_connect, 0);
	rb_define_singleton_method(rb_cFbDatabase, "connect", database_s_connect, -1);
	rb_define_method(rb_cFbDatabase, "drop", database_drop, 0);
	rb_define_singleton_method(rb_cFbDatabase, "drop", database_s_drop, -1);

	rb_cFbConnection = rb_define_class_under(rb_mFb, "Connection", rb_cData);
	rb_define_attr(rb_cFbConnection, "database", 1, 1);
	rb_define_attr(rb_cFbConnection, "username", 1, 1);
	rb_define_attr(rb_cFbConnection, "password", 1, 1);
	rb_define_attr(rb_cFbConnection, "charset", 1, 1);
	rb_define_attr(rb_cFbConnection, "role", 1, 1);
	rb_define_method(rb_cFbConnection, "to_s", connection_to_s, 0);
	rb_define_method(rb_cFbConnection, "execute", connection_execute, -1);
	rb_define_method(rb_cFbConnection, "transaction", global_transaction, -1);
	rb_define_method(rb_cFbConnection, "transaction_started", global_transaction_started, 0);
	rb_define_method(rb_cFbConnection, "commit", global_commit, 0);
	rb_define_method(rb_cFbConnection, "rollback", global_rollback, 0);
	rb_define_method(rb_cFbConnection, "close", connection_close, 0);
	rb_define_method(rb_cFbConnection, "drop", connection_drop, 0);
	rb_define_method(rb_cFbConnection, "open?", connection_is_open, 0);
	rb_define_method(rb_cFbConnection, "dialect", connection_dialect, 0);
	rb_define_method(rb_cFbConnection, "db_dialect", connection_db_dialect, 0);
	rb_define_method(rb_cFbConnection, "table_names", connection_table_names, 0);
	rb_define_method(rb_cFbConnection, "generator_names", connection_generator_names, 0);
	rb_define_method(rb_cFbConnection, "view_names", connection_view_names, 0);
	rb_define_method(rb_cFbConnection, "role_names", connection_role_names, 0);
	rb_define_method(rb_cFbConnection, "procedure_names", connection_procedure_names, 0);
	//rb_define_method(rb_cFbConnection, "cursor", connection_cursor, 0);

	rb_cFbCursor = rb_define_class_under(rb_mFb, "Cursor", rb_cData);
	//rb_define_method(rb_cFbCursor, "execute", cursor_execute, -1);
	rb_define_method(rb_cFbCursor, "fields", cursor_fields, -1);
	rb_define_method(rb_cFbCursor, "fetch", cursor_fetch, -1);
	rb_define_method(rb_cFbCursor, "fetchall", cursor_fetchall, -1);
	rb_define_method(rb_cFbCursor, "each", cursor_each, -1);
	rb_define_method(rb_cFbCursor, "close", cursor_close, 0);
	rb_define_method(rb_cFbCursor, "drop", cursor_drop, 0);

	rb_eFbError = rb_define_class_under(rb_mFb, "Error", rb_eStandardError);
	rb_define_method(rb_eFbError, "error_code", error_error_code, 0);

	rb_sFbField = rb_struct_define("Field", "name", "sql_type", "sql_subtype", "display_size", "internal_size", "precision", "scale", "nullable", "type_code", NULL);
}
