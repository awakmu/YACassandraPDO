/*
 *  Copyright 2011 DataStax
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  
 *      http://www.apache.org/licenses/LICENSE-2.0
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.                 
 */

#include "php_pdo_cassandra.hpp"
#include "php_pdo_cassandra_int.hpp"

/** {{{ static int pdo_cassandra_stmt_execute(pdo_stmt_t *stmt TSRMLS_DC)
*/
static int pdo_cassandra_stmt_execute(pdo_stmt_t *stmt TSRMLS_DC)
{
	pdo_cassandra_stmt *S = static_cast <pdo_cassandra_stmt *>(stmt->driver_data);
	pdo_cassandra_db_handle *H = static_cast <pdo_cassandra_db_handle *>(S->H);

	try {
		if (!H->transport->isOpen()) {
			H->transport->open();
		}

		std::string query(stmt->active_query_string);
		pdo_cassandra_set_active_keyspace(H, query);

		S->result.reset(new CqlResult);
		H->client->execute_cql_query(*S->result.get(), query, (H->compression ? Compression::GZIP : Compression::NONE));
		S->has_iterator = 0;
		stmt->row_count = S->result.get()->rows.size();
		return 1;
	} catch (NotFoundException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_NOT_FOUND, "%s", e.what());
	} catch (InvalidRequestException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_INVALID_REQUEST, "%s", e.why.c_str());
	} catch (UnavailableException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_UNAVAILABLE, "%s", e.what());
	} catch (TimedOutException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_TIMED_OUT, "%s", e.what());
	} catch (AuthenticationException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_AUTHENTICATION_ERROR, "%s", e.why.c_str());
	} catch (AuthorizationException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_AUTHORIZATION_ERROR, "%s", e.why.c_str());
	} catch (SchemaDisagreementException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_SCHEMA_DISAGREEMENT, "%s", e.what());
	} catch (TTransportException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_TRANSPORT_ERROR, "%s", e.what());
	} catch (TException &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_GENERAL_ERROR, "%s", e.what());
	} catch (std::exception &e) {
		pdo_cassandra_error(stmt->dbh, PDO_CASSANDRA_GENERAL_ERROR, "%s", e.what());
	}
	return 0;
}
/* }}} */

/** {{{ static int pdo_cassandra_stmt_fetch(pdo_stmt_t *stmt, enum pdo_fetch_orientation ori, long offset TSRMLS_DC)
*/
static int pdo_cassandra_stmt_fetch(pdo_stmt_t *stmt, enum pdo_fetch_orientation ori, long offset TSRMLS_DC)
{
	pdo_cassandra_stmt *S = static_cast <pdo_cassandra_stmt *>(stmt->driver_data);

	if (!S->result.get()->rows.size ())
		return 0;

	if (!S->has_iterator) {
		S->it = S->result.get()->rows.begin();
		S->has_iterator = 1;
		stmt->column_count = (*S->it).columns.size();
	} else {
		S->it++;
	}

	if (S->it == S->result.get()->rows.end()) {
		// Iterated all rows, reset the iterator
		S->has_iterator = 0;
		return 0;
	}
	return 1;
}
/* }}} */

/** {{{ static int pdo_cassandra_stmt_describe(pdo_stmt_t *stmt, int colno TSRMLS_DC)
*/
static int pdo_cassandra_stmt_describe(pdo_stmt_t *stmt, int colno TSRMLS_DC)
{
	pdo_cassandra_stmt *S = static_cast <pdo_cassandra_stmt *>(stmt->driver_data);
	pdo_cassandra_db_handle *H = static_cast <pdo_cassandra_db_handle *>(S->H);

	if (colno < 0 || (colno >= 0 && (static_cast <size_t>(colno) >= (*S->it).columns.size()))) {
		return 0;
	}

	if (!(*S->it).columns[colno].name.size()) {
		return 0;
	}

	KsDef description;
	H->client->describe_keyspace(description, H->active_keyspace);

	stmt->columns[colno].param_type = PDO_PARAM_STR;

	for (unsigned int i = 0; i < description.cf_defs.size (); i++)
	{
		for (unsigned int j = 0; j < description.cf_defs [i].column_metadata.size (); j++)
		{
			if ((*S->it).columns[colno].name == description.cf_defs [i].column_metadata [j].name)
			{
				if (!description.cf_defs [i].column_metadata [j].validation_class.compare("org.apache.cassandra.db.marshal.LongType") ||
					!description.cf_defs [i].column_metadata [j].validation_class.compare("org.apache.cassandra.db.marshal.IntegerType"))
				{
					stmt->columns[colno].param_type = PDO_PARAM_INT;
				}
				else if (!description.cf_defs [i].column_metadata [j].validation_class.compare("org.apache.cassandra.db.marshal.BytesType"))
				{
					stmt->columns[colno].param_type = PDO_PARAM_LOB;
				}
				else
				{
					stmt->columns[colno].param_type = PDO_PARAM_STR;
				}
			}			
		}
	}

	stmt->columns[colno].name       = estrdup (const_cast <char *> ((*S->it).columns[colno].name.c_str()));
	stmt->columns[colno].namelen    = (*S->it).columns[colno].name.size();
	stmt->columns[colno].maxlen     = -1;
	stmt->columns[colno].precision  = 0;

	return 1;
}
/* }}} */

/** {{{ static int pdo_cassandra_stmt_get_column(pdo_stmt_t *stmt, int colno, char **ptr, unsigned long *len, int *caller_frees TSRMLS_DC)
*/
static int pdo_cassandra_stmt_get_column(pdo_stmt_t *stmt, int colno, char **ptr, unsigned long *len, int *caller_frees TSRMLS_DC)
{
	pdo_cassandra_stmt *S = static_cast <pdo_cassandra_stmt *>(stmt->driver_data);

	if (colno < 0 || (colno >= 0 && (static_cast <size_t>(colno) >= (*S->it).columns.size()))) {
		return 0;
	}

	*ptr          = const_cast <char *> ((*S->it).columns[colno].value.c_str());
	*len          = (*S->it).columns[colno].value.size();
	*caller_frees = 0;
	return 1;
}
/* }}} */

/** {{{ static int pdo_cassandra_stmt_get_column_meta(pdo_stmt_t *stmt, long colno, zval *return_value TSRMLS_DC)
*/
static int pdo_cassandra_stmt_get_column_meta(pdo_stmt_t *stmt, long colno, zval *return_value TSRMLS_DC)
{
	pdo_cassandra_stmt *S = static_cast <pdo_cassandra_stmt *>(stmt->driver_data);
	pdo_cassandra_db_handle *H = static_cast <pdo_cassandra_db_handle *>(S->H);

	if (colno < 0 || (colno >= 0 && (static_cast <size_t>(colno) >= (*S->it).columns.size()))) {
		return FAILURE;
	}

	KsDef description;
	H->client->describe_keyspace(description, H->active_keyspace);

	array_init(return_value);

	bool found = false;
	for (unsigned int i = 0; i < description.cf_defs.size (); i++)
	{
		for (unsigned int j = 0; j < description.cf_defs [i].column_metadata.size (); j++)
		{
			if ((*S->it).columns[colno].name.compare(description.cf_defs [i].column_metadata [j].name) == 0)
			{
				found = true;
				add_assoc_string(return_value,
				                 "native_type",
								 const_cast <char *> (description.cf_defs [i].column_metadata [j].validation_class.c_str()),
								 1);
				add_assoc_string(return_value,
				                 "comparator",
								 const_cast <char *> (description.cf_defs [i].comparator_type.c_str()),
								 1);
				add_assoc_string(return_value,
				                 "default_validation_class",
								 const_cast <char *> (description.cf_defs [i].default_validation_class.c_str()),
								 1);
				add_assoc_string(return_value,
				                 "key_validation_class",
								 const_cast <char *> (description.cf_defs [i].key_validation_class.c_str()),
								 1);
				add_assoc_string(return_value,
				                 "key_alias",
								 const_cast <char *> (description.cf_defs [i].key_alias.c_str()),
								 1);
			}
		}
	}

	if (!found) {
		add_assoc_string(return_value,
		                 "native_type",
						 "unknown",
						 1);
	}
	return SUCCESS;
}
/* }}} */

/** {{{ static int pdo_cassandra_stmt_cursor_closer(pdo_stmt_t *stmt TSRMLS_DC)
*/
static int pdo_cassandra_stmt_cursor_close(pdo_stmt_t *stmt TSRMLS_DC)
{
	pdo_cassandra_stmt *S = static_cast <pdo_cassandra_stmt *>(stmt->driver_data);
	S->has_iterator       = 0;
	return 1;
}
/* }}} */

/** {{{ static int pdo_cassandra_stmt_dtor(pdo_stmt_t *stmt TSRMLS_DC)
*/
static int pdo_cassandra_stmt_dtor(pdo_stmt_t *stmt TSRMLS_DC)
{
	if (stmt->driver_data) {
		pdo_cassandra_stmt *S = static_cast <pdo_cassandra_stmt *>(stmt->driver_data);
		S->result.reset();
		delete S;
		stmt->driver_data = NULL;
	}
	return 1;
}
/* }}} */

struct pdo_stmt_methods cassandra_stmt_methods = {
	pdo_cassandra_stmt_dtor,
	pdo_cassandra_stmt_execute,
	pdo_cassandra_stmt_fetch,
	pdo_cassandra_stmt_describe,
	pdo_cassandra_stmt_get_column,
	NULL,
	NULL, /* set_attr */
	NULL, /* get_attr */
	pdo_cassandra_stmt_get_column_meta,
	NULL, /* next_rowset */
	pdo_cassandra_stmt_cursor_close
};

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
