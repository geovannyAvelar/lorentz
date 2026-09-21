/* Lorentz: A black hole for Internet advertisements
*  (c) 2023 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  Lorentz Engine
*  Teleporter un-/compression routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "lorentz.h"
#include "zip/teleporter.h"
#include "config/config.h"
// hostname()
#include "daemon.h"
// get_timestr(), TIMESTR_SIZE
#include "log.h"
// directory_exists()
#include "files.h"
// DIR, dirent, opendir(), readdir(), closedir()
#include <dirent.h>
// db_open_ex()
#include "database/db-driver.h"
// toml_parse()
#include "config/tomlc17/tomlc17.h"
// readLorentztoml()
#include "config/toml_reader.h"
// writeLorentztoml()
#include "config/toml_writer.h"
// write_dnsmasq_config()
#include "config/dnsmasq_config.h"
// lock_shm(), unlock_shm()
#include "shmem.h"
// rotate_file()
#include "files.h"
// cJSON
#include "webserver/cJSON/cJSON.h"
// set_event()
#include "events.h"
// JSON_KEY_TRUE
#include "webserver/json_macros.h"
// exit_code
#include "signals.h"
// sqliteBusyCallback()
#include "database/common.h"

// Tables to copy from the gravity database to the Teleporter database
static const char *gravity_tables[] = {
	"group",
	"adlist",
	"adlist_by_group",
	"domainlist",
	"domainlist_by_group",
	"client",
	"client_by_group"
};

// Tables to copy from the Lorentz database to the Teleporter database
static const char *lorentz_tables[] = {
	"message",
	"aliasclient",
	"network",
	"network_addresses"
};

// Copy a message into the ERRBUF_SIZE-sized hint buffer, always leaving it
// NUL-terminated (plain strncpy(hint, src, ERRBUF_SIZE) would not terminate
// when src is ERRBUF_SIZE bytes or longer, e.g. a long SQLite error message)
static void set_hint(char *hint, const char *src)
{
	if(src == NULL)
		src = "";
	strncpy(hint, src, ERRBUF_SIZE - 1);
	hint[ERRBUF_SIZE - 1] = '\0';
}

// Copy the tables of a database on a server into the in-memory database. The
// server cannot be attached, so the rows are read and inserted one by one. The
// copies have the names of the columns and nothing else
static bool copy_remote_tables(db_conn *db, const char *uri, const char **tables, const unsigned int num_tables)
{
	const char *open_error = NULL;
	db_conn *remote = db_open_uri_ex(uri, DB_OPEN_READONLY | DB_OPEN_NOMUTEX, NULL, &open_error);
	if(remote == NULL)
	{
		log_warn("Failed to open the long-term database: %s", open_error);
		return false;
	}

	bool okay = true;
	for(unsigned int i = 0; okay && i < num_tables; i++)
	{
		char sql[256];
		snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\"", tables[i]);
		db_stmt *sel = db_prepare(remote, sql, false);
		if(sel == NULL)
		{
			log_warn("Failed to read %s: %s", tables[i], db_errmsg(remote));
			okay = false;
			break;
		}
		const int ncols = db_column_count(sel);
		char create[1024];
		size_t len = (size_t)snprintf(create, sizeof(create), "CREATE TABLE \"%s\" (", tables[i]);
		char values[256] = "";
		for(int c = 0; c < ncols && len < sizeof(create) - 64; c++)
		{
			len += (size_t)snprintf(create + len, sizeof(create) - len, "%s\"%s\"", c > 0 ? ", " : "", db_column_name(sel, c));
			snprintf(values + strlen(values), sizeof(values) - strlen(values), "%s?", c > 0 ? "," : "");
		}
		snprintf(create + len, sizeof(create) - len, ")");
		snprintf(sql, sizeof(sql), "INSERT INTO \"%s\" VALUES (%s)", tables[i], values);
		db_stmt *ins = NULL;
		if(db_exec(db, create) != DB_OK || (ins = db_prepare(db, sql, false)) == NULL)
		{
			log_warn("Failed to create %s in in-memory database: %s", tables[i], db_errmsg(db));
			okay = false;
		}
		else if(db_copy_rows(sel, ins, db, ncols, -1, tables[i]) < 0)
		{
			log_warn("Failed to copy %s to in-memory database", tables[i]);
			okay = false;
		}
		db_finalize(ins);
		db_finalize(sel);
	}

	db_close(remote);
	return okay;
}

// Create database in memory, copy selected tables to it, serialize and return a memory pointer to it
static bool create_teleporter_database(const char *filename, const char **tables, const unsigned int num_tables,
                                       void **buffer, size_t *size)
{
	// Open in-memory database
	const char *open_error = NULL;
	db_conn *db = db_open_sqlite_ex(":memory:", DB_OPEN_READWRITE | DB_OPEN_NOMUTEX, NULL, &open_error);
	if(db == NULL)
	{
		log_warn("Failed to open in-memory database: %s", open_error);
		return false;
	}

	// Set busy timeout to access the database in a
	// multi-threaded environment
	if(db_set_busy_handler(db, sqliteBusyCallback, NULL) != DB_OK)
		log_warn("Failed to set busy timeout during creation of in-memory Teleporter database: %s", db_errmsg(db));

	// A database on a server is copied, not attached
	if(db_uri_is_remote(filename))
	{
		if(!copy_remote_tables(db, filename, tables, num_tables))
		{
			db_close(db);
			return false;
		}
	}
	// Attach the Lorentz database to the in-memory database
	else if(db_attach(db, filename, "disk") != DB_OK)
	{
		log_warn("Failed to attach database \"%s\" to in-memory database: %s", filename, db_errmsg(db));
		db_close(db);
		return false;
	}

	// Loop over the tables and copy them to the in-memory database
	for(unsigned int i = 0; !db_uri_is_remote(filename) && i < num_tables; i++)
	{
		char create_stmt[128] = "";

		// Create in-memory table copy
		snprintf(create_stmt, sizeof(create_stmt), "CREATE TABLE \"%s\" AS SELECT * FROM disk.\"%s\";", tables[i], tables[i]);
		if(db_exec(db, create_stmt) != DB_OK)
		{
			log_warn("Failed to create %s in in-memory database: %s", tables[i], db_errmsg(db));
			db_close(db);
			return false;
		}
	}

	// Detach the Lorentz database from the in-memory database
	if(!db_uri_is_remote(filename) && db_detach(db, "disk") != DB_OK)
	{
		log_warn("Failed to detach Lorentz database from in-memory database: %s", db_errmsg(db));
		db_close(db);
		return false;
	}

	// Serialize the in-memory database to a buffer
	// The serialization interface returns a pointer to memory that
	// is a serialization of the S database on database connection D. If P is
	// not a NULL pointer, then the size of the database in bytes is written
	// into *P.
	// For an ordinary on-disk database file, the serialization is just a copy
	// of the disk file. For an in-memory database or a "TEMP" database, the
	// serialization is the same sequence of bytes which would be written to
	// disk if that database where backed up to disk.
	// The usual case is that the serialization is copied into memory owned by
	// the database driver. The caller is responsible for releasing the
	// returned buffer with db_free_buffer() to avoid a memory leak.
	int64_t isize = 0;
	*buffer = db_serialize(db, "main", &isize);
	*size = isize;
	if(*buffer == NULL)
	{
		log_warn("Failed to serialize in-memory database to buffer: %s", db_errmsg(db));
		db_close(db);
		return false;
	}

	// Close the in-memory database
	db_close(db);

	return true;
}

const char *generate_teleporter_zip(mz_zip_archive *zip, char filename[128], void **ptr, size_t *size)
{
	// Initialize ZIP archive
	memset(zip, 0, sizeof(*zip));

	// Start with 64KB allocation size (lorentz.TOML is slightly larger than 32KB
	// at the time of writing thjs)
	if(!mz_zip_writer_init_heap(zip, 0, 64*1024))
	{
		return "Failed creating heap ZIP archive";
	}

	// Add lorentz.toml to the ZIP archive
	const char *file_comment = "Lorentz's configuration";
	const char *file_path = GLOBALTOMLPATH;
	if(!mz_zip_writer_add_file(zip, file_path+1, file_path, file_comment, (uint16_t)strlen(file_comment), MZ_BEST_COMPRESSION))
	{
		mz_zip_writer_end(zip);
		return "Failed to add "GLOBALTOMLPATH" to heap ZIP archive!";
	}

	// Add /etc/hosts to the ZIP archive
	file_comment = "System's HOSTS file";
	file_path = "/etc/hosts";
	if(!mz_zip_writer_add_file(zip, file_path+1, file_path, file_comment, (uint16_t)strlen(file_comment), MZ_BEST_COMPRESSION))
	{
		mz_zip_writer_end(zip);
		return "Failed to add /etc/hosts to heap ZIP archive!";
	}

	// Add /etc/lorentz/dhcp.lease to the ZIP archive if it exists
	file_comment = "DHCP leases file";
	file_path = "/etc/lorentz/dhcp.leases";
	if(file_exists(file_path) && !mz_zip_writer_add_file(zip, file_path+1, file_path, file_comment, (uint16_t)strlen(file_comment), MZ_BEST_COMPRESSION))
	{
		mz_zip_writer_end(zip);
		return "Failed to add /etc/lorentz/dhcp.leases to heap ZIP archive!";
	}

	const char *directory = "/etc/dnsmasq.d";
	if(directory_exists(directory))
	{
		// Loop over all files and add them to the ZIP archive
		DIR *dir;
		if((dir = opendir(directory)) != NULL)
		{
			// Loop over all files in the directory
			struct dirent *ent;
			while((ent = readdir(dir)) != NULL)
			{
				// Skip "." and ".."
				if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
					continue;

				// Construct full path to file
				char fullpath[128] = "";
				snprintf(fullpath, 128, "%s/%s", directory, ent->d_name);

				// Add file to ZIP archive
				file_comment = "dnsmasq configuration file";
				file_path = fullpath;

				if(!mz_zip_writer_add_file(zip, file_path+1, file_path, file_comment, (uint16_t)strlen(file_comment), MZ_BEST_COMPRESSION))
					continue;
			}
			closedir(dir);
		}
	}

	// Add (a reduced version of) the gravity database to the ZIP archive
	void *dbbuf = NULL;
	size_t dbsize = 0u;
	if(create_teleporter_database(config.files.gravity.v.s, gravity_tables, ArraySize(gravity_tables), &dbbuf, &dbsize))
	{
		// Add gravity database to ZIP archive
		file_comment = "Lorentz's gravity database";
		file_path = config.files.gravity.v.s;
		if(file_path[0] == '/')
			file_path++;
		if(!mz_zip_writer_add_mem_ex(zip, file_path, dbbuf, dbsize, file_comment, (uint16_t)strlen(file_comment), MZ_BEST_COMPRESSION, 0, 0))
		{
			db_free_buffer(dbbuf);
			mz_zip_writer_end(zip);
			return "Failed to add gravity database to heap ZIP archive!";
		}
		db_free_buffer(dbbuf);
	}
	else
	{
		mz_zip_writer_end(zip);
		return "Failed to create gravity database for heap ZIP archive!";
	}

	if(create_teleporter_database(config.files.database.v.s, lorentz_tables, ArraySize(lorentz_tables), &dbbuf, &dbsize))
	{
		// Add Lorentz database to ZIP archive
		file_comment = "Lorentz's database";
		// A database on a server has no path to name the entry after
		file_path = db_uri_is_remote(config.files.database.v.s) ? "etc/lorentz/lorentz.db" : config.files.database.v.s;
		if(file_path[0] == '/')
			file_path++;
		if(!mz_zip_writer_add_mem_ex(zip, file_path, dbbuf, dbsize, file_comment, (uint16_t)strlen(file_comment), MZ_BEST_COMPRESSION, 0, 0))
		{
			db_free_buffer(dbbuf);
			mz_zip_writer_end(zip);
			return "Failed to add Lorentz database to heap ZIP archive!";
		}
		db_free_buffer(dbbuf);
	}
	else
	{
		mz_zip_writer_end(zip);
		return "Failed to create Lorentz database for heap ZIP archive!";
	}

	// Get the heap data so we can send it to the requesting client
	if(!mz_zip_writer_finalize_heap_archive(zip, ptr, size))
	{
		mz_zip_writer_end(zip);
		return "Failed to finalize heap ZIP archive!";
	}

	// Verify that the ZIP archive is valid
	mz_zip_error pErr;
	if(!mz_zip_validate_mem_archive(*ptr, *size, MZ_ZIP_FLAG_VALIDATE_LOCATE_FILE_FLAG, &pErr))
	{
		log_warn("Failed to validate generated Teleporter ZIP archive: %s",
		         mz_zip_get_error_string(pErr));
	}

	// Generate filename for ZIP archive (it has both the hostname and the
	// current datetime)
	char timestr[TIMESTR_SIZE];
	get_timestr(timestr, time(NULL), false, true);
	snprintf(filename, 128, "lorentz_%s_teleporter_%s.zip", hostname(), timestr);

	// Everything worked well
	return NULL;
}

static const char *test_and_import_lorentz_toml(void *ptr, size_t size, char * const hint)
{
	// Check if the file is empty
	if(size == 0)
		return "File etc/lorentz/lorentz.toml in ZIP archive is empty";

	// Create a memory copy that is null-terminated
	char *buffer = calloc(size+1, sizeof(char));
	if(buffer == NULL)
		return "Failed to allocate memory for null-terminated copy of etc/lorentz/lorentz.toml in ZIP archive";
	memcpy(buffer, ptr, size);
	buffer[size] = '\0';

	// Check if the file is a valid TOML file
	toml_result_t toml = toml_parse(buffer, size);
	if(!toml.ok)
	{
		free(buffer);
		log_err("ZIP TOML file is not valid: %s", toml.errmsg);
		return "File etc/lorentz/lorentz.toml in ZIP archive is not a valid TOML file";
	}
	free(buffer);

	// Check if the file contains a valid configuration for Lorentz by parsing it into
	// a temporary config struct (teleporter_config)
	struct config teleporter_config = { 0 };
	duplicate_config(&teleporter_config, &config);
	// readLorentztoml() holds every value in the archive to the validator its config
	// item declares. An import is not a lesser path than PATCH /api/config: it
	// is reachable by anyone holding an admin session and installs a complete
	// configuration, so a value the API refuses must not get in this way either.
	char valerr[VALIDATOR_ERRBUF_LEN] = { 0 };
	if(!readLorentztoml(NULL, &teleporter_config, toml.toptab, true, NULL, 0, true, valerr))
	{
		free_config(&teleporter_config, false);
		toml_free(toml);

		// The buffer names the offending item when a value was refused, and
		// stays empty when the file could not be read at all
		if(valerr[0] == '\0')
			return "File etc/lorentz/lorentz.toml in ZIP archive contains invalid TOML configuration";

		log_err("Teleporter: %s", valerr);
		set_hint(hint, valerr);
		return "File etc/lorentz/lorentz.toml in ZIP archive contains an invalid value";
	}

	// Test dnsmasq config in the imported configuration
	// The dnsmasq configuration will be overwritten if the test succeeds
	if(!write_dnsmasq_config(&teleporter_config, DNSMASQ_TEST_INSTALL, hint))
	{
		free_config(&teleporter_config, false);
		toml_free(toml);
		return "File etc/lorentz/lorentz.toml in ZIP archive contains invalid dnsmasq configuration";
	}

	// When we reach this point, we know that the file is a valid TOML file and contains
	// a valid configuration for Lorentz. We can now safely overwrite the current
	// configuration with the one from the ZIP archive

	// Install new configuration (takes ownership of teleporter_config)
	replace_config(&teleporter_config);

	// Write new lorentz.toml to disk, the dnsmaq config was already written above
	// Also write the custom list to disk
	rotate_files(GLOBALTOMLPATH, NULL);
	writeLorentztoml(true, NULL);
	write_custom_list();

	toml_free(toml);
	return NULL;
}

// Check that an imported DHCP lease database actually looks like one.
//
// The archive member is written verbatim to a well-known path, which makes the
// import a way to place chosen bytes on disk. Rejecting anything that is not
// printable ASCII, and any record not opening with a type dnsmasq knows, keeps
// an arbitrary file from arriving under a name dnsmasq will parse.
//
// This is a shape check, not a grammar check: the fields after the first are
// not validated, so a well-formed record can still carry arbitrary printable
// text. Accepted records are "duid <hex>", "vendorclass|agent-info <address>
// <hex>" and "<expiry> <hwaddr> <address> [hostname [clientid]]".
bool valid_dhcp_leases(const char *data, const size_t size)
{
	size_t pos = 0;
	while(pos < size)
	{
		// Determine the extent of this line
		size_t eol = pos;
		while(eol < size && data[eol] != '\n')
			eol++;

		// A lease database holds only numbers, hex, addresses and host
		// names, so anything outside printable ASCII is not one
		for(size_t i = pos; i < eol; i++)
			if(data[i] != '\t' && (data[i] < 0x20 || data[i] > 0x7e))
				return false;

		// Skip leading whitespace, accept empty lines
		while(pos < eol && (data[pos] == ' ' || data[pos] == '\t'))
			pos++;
		if(pos == eol)
		{
			pos = eol + 1;
			continue;
		}

		// The first token decides the record type
		size_t tok = pos;
		while(tok < eol && data[tok] != ' ' && data[tok] != '\t')
			tok++;
		const size_t toklen = tok - pos;

		bool numeric = true;
		for(size_t i = pos; i < tok; i++)
			if(data[i] < '0' || data[i] > '9')
				numeric = false;

		if(!numeric &&
		   !(toklen == 4 && strncmp(data + pos, "duid", 4) == 0) &&
		   !(toklen == 11 && strncmp(data + pos, "vendorclass", 11) == 0) &&
		   !(toklen == 10 && strncmp(data + pos, "agent-info", 10) == 0))
			return false;

		pos = eol + 1;
	}

	return true;
}

static const char *import_dhcp_leases(const void *ptr, size_t size, char * const hint)
{
	// We do not check if the file is empty here, as an empty dhcp.leases file is valid

	// Check the content really is a lease database before overwriting the
	// current one - the bytes come straight from the uploaded archive.
	//
	// Skip the file rather than failing the import: lorentz.toml is installed
	// earlier in the same archive, so returning an error here would report
	// failure for an import that has already changed the configuration. The
	// TAR.GZ importer skips the same file for the same reason.
	if(!valid_dhcp_leases(ptr, size))
	{
		log_warn("Not importing etc/lorentz/dhcp.leases: not a DHCP lease database");
		return NULL;
	}

	// Rotate the current dhcp.leases file to keep a backup of the previous version

	// Rotate current dhcp.leases file
	rotate_files(DHCPLEASESFILE, NULL);

	// Write new dhcp.leases file to disk
	FILE *fp = fopen(DHCPLEASESFILE, "w");
	if(fp == NULL)
	{
		set_hint(hint, strerror(errno));
		return "Failed to open dhcp.leases file for writing";
	}
	if(fwrite(ptr, 1, size, fp) != size)
	{
		set_hint(hint, strerror(errno));
		fclose(fp);
		return "Failed to write to dhcp.leases file";
	}
	fclose(fp);

	return NULL;
}

static const char *test_and_import_database(void *ptr, size_t size, const char *destination,
                                            const char **tables, const size_t num_tables,
                                            char * const hint)
{
	// Check if the file is empty
	// The first 100 bytes of the database file comprise the database file header.
	// See https://www.sqlite.org/fileformat.html, section 1.3
	if(size < 100)
	{
		return "File etc/lorentz/gravity.db in ZIP archive is empty";
	}

	// Check file header to see if this is a SQLite3 database file
	// Every valid SQLite database file begins with the following 16 bytes (in
	// hex): 53 51 4c 69 74 65 20 66 6f 72 6d 61 74 20 33 00. This byte sequence
	// corresponds to the UTF-8 string "SQLite format 3" including the nul
	// terminator character at the end. The nul terminator character is not
	// included in the 16 bytes of the header.
	// See https://www.sqlite.org/fileformat.html, section 1.3.1
	if(memcmp(ptr, "SQLite format 3", 15) != 0)
	{
		return "File etc/lorentz/gravity.db in ZIP archive is not a SQLite3 database file (no header)";
	}

	// Check if the file is a valid SQlite3 database
	// We do this by trying to deserialize the file into a database object. If
	// this fails, the file is not a valid SQlite3 database. The buffer is read
	// in place and never modified, it outlives the connection
	const char *open_error = NULL;
	db_conn *database = db_open_sqlite_ex(":memory:", DB_OPEN_READWRITE | DB_OPEN_NOMUTEX, NULL, &open_error);
	if(database == NULL)
	{
		set_hint(hint, open_error);
		return "Failed to open temporary SQLite3 database";
	}
	if(db_deserialize(database, "main", ptr, size, true) != DB_OK)
	{
		set_hint(hint, db_errmsg(database));
		db_close(database);
		return "File etc/lorentz/gravity.db in ZIP archive is not a valid SQLite3 database file";
	}

	// Run PRAGMA integrity_check on the database to check if the database is
	// valid. If the database is valid, the result of the PRAGMA integrity_check
	// is "ok". If the database is invalid, the result of the PRAGMA
	// integrity_check is a string describing the error.
	// See https://www.sqlite.org/pragma.html#pragma_integrity_check
	db_stmt *statement = db_prepare(database, "PRAGMA integrity_check;", false);
	if(statement == NULL)
	{
		set_hint(hint, db_errmsg(database));
		db_close(database);
		return "Failed to prepare PRAGMA integrity_check statement";
	}
	if(db_step(statement) != DB_ROW)
	{
		set_hint(hint, db_errmsg(database));
		db_finalize(statement);
		db_close(database);
		return "Failed to execute PRAGMA integrity_check statement";
	}
	if(strcmp(db_column_text(statement, 0), "ok") != 0)
	{
		set_hint(hint, db_column_text(statement, 0));
		db_finalize(statement);
		db_close(database);
		return "Database file in ZIP archive is not a valid SQLite3 database (integrity check failed)";
	}
	// Finalize statement
	db_finalize(statement);

	// When we reach this point, we know that the file is a valid SQLite3 database

	// ATTACH the database file to the in-memory database
	if(db_attach(database, destination, "disk") != DB_OK)
	{
		set_hint(hint, db_errmsg(database));
		db_close(database);
		return "Failed to attach database file to in-memory SQLite3 database";
	}

	// Disable foreign key checks for import
	if(db_exec(database, "PRAGMA foreign_keys = 0;") != DB_OK)
	{
		set_hint(hint, db_errmsg(database));
		db_close(database);
		return "Failed to disable foreign key checks for import";
	}

	// Start transaction
	if(db_begin(database, DB_TX_DEFERRED) != DB_OK)
	{
		set_hint(hint, db_errmsg(database));
		db_close(database);
		return "Failed to start transaction";
	}

	// Loop over the tables
	for(unsigned int i = 0; i < num_tables; i++)
	{
		char stmt[256] = "";
		// Delete all rows in the disk table
		snprintf(stmt, sizeof(stmt), "DELETE FROM disk.\"%s\";", tables[i]);
		if(db_exec(database, stmt) != DB_OK)
		{
			set_hint(hint, db_errmsg(database));
			db_close(database);
			return "Failed to delete from disk database table";
		}

		// Store the table in the disk database
		// We have to use INSERT OR REPLACE here, because the gravity database
		// has several triggers, e.g., immediately recreating the default group
		// on (accidental) deletion. This would cause the import to fail due to
		// a unique constraint violation.
		snprintf(stmt, sizeof(stmt), "INSERT OR REPLACE INTO disk.\"%s\" SELECT * FROM \"%s\";", tables[i], tables[i]);
		if(db_exec(database, stmt) != DB_OK)
		{
			set_hint(hint, db_errmsg(database));
			db_close(database);
			return "Failed to insert into disk database table";
		}

		log_debug(DEBUG_DATABASE, "Replaced table %s in %s", tables[i], destination);
	}

	// End transaction
	if(db_commit(database) != DB_OK)
	{
		set_hint(hint, db_errmsg(database));
		db_close(database);
		return "Failed to commit transaction";
	}

	// Detach the database file from the in-memory database
	if(db_detach(database, "disk") != DB_OK)
	{
		set_hint(hint, db_errmsg(database));
		db_close(database);
		return "Failed to detach database file from in-memory SQLite3 database";
	}

	// Close the database
	db_close(database);

	// Add event to reload gravity database
	set_event(RELOAD_GRAVITY);

	return NULL;
}

const char *read_teleporter_zip(uint8_t *buffer, const size_t buflen, char * const hint, cJSON *import, cJSON *imported_files)
{
	// Initialize ZIP archive
	mz_zip_archive zip = { 0 };
	memset(&zip, 0, sizeof(zip));

	log_debug(DEBUG_CONFIG, "Reading ZIP archive from memory buffer (size %zu)", buflen);

	// Open ZIP archive from memory buffer
	if(!mz_zip_reader_init_mem(&zip, buffer, buflen, 0))
	{
		set_hint(hint, mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
		return "Failed to parse received ZIP archive";
	}

	// Loop over all files in the ZIP archive
	for(mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++)
	{
		// Get file information
		mz_zip_archive_file_stat file_stat;
		if(!mz_zip_reader_file_stat(&zip, i, &file_stat))
		{
			log_warn("Failed to get file information for file %u in ZIP archive: %s",
			         i, mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
			continue;
		}

		// List of files to process from a Teleporter ZIP archive
		const char *extract_files[] = {
			"etc/lorentz/lorentz.toml",
			"etc/lorentz/dhcp.leases",
			config.files.gravity.v.s[0] == '/' ? config.files.gravity.v.s + 1 : config.files.gravity.v.s
		};

		// Check if this file is one of the files we want to extract and process
		bool extract = false;
		for(size_t j = 0; j < ArraySize(extract_files); j++)
		{
			if(strcmp(file_stat.m_filename, extract_files[j]) == 0)
			{
				extract = true;
				break;
			}
		}
		if(!extract)
		{
			log_info("Skipping file %s in Teleporter archive", file_stat.m_filename);
			continue;
		}

		// Reject an absurdly large (attacker-controlled) uncompressed size
		// before allocating, to avoid a memory-exhaustion DoS. This matches
		// the 256 MiB cap enforced on the gzip path.
		if(file_stat.m_uncomp_size > 0x10000000)
		{
			log_warn("Skipping file %u (%s) in ZIP archive: uncompressed size %llu is too large",
			         i, file_stat.m_filename, (unsigned long long)file_stat.m_uncomp_size);
			continue;
		}

		// Read file into its dedicated memory buffer
		void *ptr = malloc(file_stat.m_uncomp_size);
		if(ptr == NULL)
		{
			log_warn("Failed to allocate memory for file %u (%s) in ZIP archive: %s",
			         i, file_stat.m_filename, mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
			continue;
		}
		if(!mz_zip_reader_extract_to_mem(&zip, i, ptr, file_stat.m_uncomp_size, 0))
		{
			log_warn("Failed to read file %u (%s) in ZIP archive: %s",
			         i, file_stat.m_filename, mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
			free(ptr);
			continue;
		}

		log_debug(DEBUG_CONFIG, "Processing file %u (%s) in ZIP archive (%zu/%zu bytes, comment: \"%s\", timestamp: %lu)",
		          i, file_stat.m_filename, (size_t)file_stat.m_comp_size, (size_t)file_stat.m_uncomp_size,
		          file_stat.m_comment, (unsigned long)file_stat.m_time);

		// Process file
		const char *import_tables[ArraySize(gravity_tables)] = { NULL };
		size_t num_tables = 0u;
		// Is this "etc/lorentz/lorentz.toml" ?
		if(strcmp(file_stat.m_filename, extract_files[0]) == 0)
		{
			// Check whether we should import this file
			if(import != NULL && !JSON_KEY_TRUE(import, "config"))
			{
				log_info("Ignoring file %s in Teleporter archive (not in import list)", file_stat.m_filename);
				free(ptr);
				continue;
			}

			// Import Lorentz configuration
			memset(hint, 0, ERRBUF_SIZE);
			const char *err = test_and_import_lorentz_toml(ptr, file_stat.m_uncomp_size, hint);
			if(err != NULL)
			{
				free(ptr);
				mz_zip_reader_end(&zip);
				return err;
			}
			log_debug(DEBUG_CONFIG, "Imported Lorentz configuration: %s", file_stat.m_filename);
		}
		// Is this "etc/lorentz/dhcp.leases"?
		else if(strcmp(file_stat.m_filename, extract_files[1]) == 0)
		{
			// Check whether we should import this file
			if(import != NULL && !JSON_KEY_TRUE(import, "dhcp_leases"))
			{
				log_info("Ignoring file %s in Teleporter archive (not in import list)", file_stat.m_filename);
				free(ptr);
				continue;
			}

			// Import DHCP leases
			memset(hint, 0, ERRBUF_SIZE);
			const char *err = import_dhcp_leases(ptr, file_stat.m_uncomp_size, hint);
			if(err != NULL)
			{
				free(ptr);
				mz_zip_reader_end(&zip);
				return err;
			}
			log_debug(DEBUG_CONFIG, "Imported DHCP leases: %s", file_stat.m_filename);
		}
		// Is this "etc/lorentz/gravity.db"?
		else if(strcmp(file_stat.m_filename, extract_files[2]) == 0)
		{
			// Check whether we should import this file
			if(import != NULL && !cJSON_HasObjectItem(import, "gravity"))
			{
				log_info("Ignoring file %s in Teleporter archive (not in import list)", file_stat.m_filename);
				free(ptr);
				continue;
			}

			if(import == NULL)
			{
				// Import all tables
				num_tables = ArraySize(gravity_tables);
				memcpy(import_tables, gravity_tables, sizeof(gravity_tables));
			}
			else
			{
				// Get object at import.gravity
				cJSON *import_gravity = cJSON_GetObjectItem(import, "gravity");

				// Check if import.gravity is a JSON object
				if(import_gravity == NULL || !cJSON_IsObject(import_gravity))
				{
					log_warn("Ignoring file %s in Teleporter archive (import.gravity is not a JSON object)", file_stat.m_filename);
					free(ptr);
					continue;
				}

				// Import selected tables from import.gravity object
				for(size_t j = 0; j < ArraySize(gravity_tables); j++)
				{
					if(JSON_KEY_TRUE(import_gravity, gravity_tables[j]))
						import_tables[num_tables++] = gravity_tables[j];
					else
						log_info("Ignoring table %s in %s (not in import list)", gravity_tables[j], file_stat.m_filename);
				}
			}

			// Import gravity database
			memset(hint, 0, ERRBUF_SIZE);
			const char *err = test_and_import_database(ptr, file_stat.m_uncomp_size, config.files.gravity.v.s,
			                                           import_tables, num_tables, hint);
			if(err != NULL)
			{
				free(ptr);
				mz_zip_reader_end(&zip);
				return err;
			}
			log_debug(DEBUG_CONFIG, "Imported database: %s", file_stat.m_filename);

			// Add filename of processed files to JSON array
			for(unsigned j = 0; j < num_tables; j++)
			{
				const size_t len = strlen(file_stat.m_filename) + 3 + strlen(import_tables[j]);
				char *tablename = calloc(len, sizeof(char));
				if(tablename == NULL)
				{
					log_err("Failed to allocate memory for table name");
					free(ptr);
					continue;
				}

				// Create imported pseudo file name in the
				// format "filename->table" and add it to the
				// JSON array
				snprintf(tablename, len, "%s->%s", file_stat.m_filename, import_tables[j]);
				if(imported_files != NULL && !cJSON_AddItemToArray(imported_files, cJSON_CreateString(tablename)))
					log_warn("Failed to add table %s to JSON array", tablename);
				free(tablename);
			}

			// Free allocated memory and skip to next file without
			// adding it to the JSON array again below
			free(ptr);
			continue;
		}
		else
		{
			log_warn("Ignoring file %s in Teleporter archive", file_stat.m_filename);

			// Free allocated memory and skip to next file
			free(ptr);
			continue;
		}

		// Add filename of processed files to JSON array
		if(imported_files != NULL && !cJSON_AddItemToArray(imported_files, cJSON_CreateString(file_stat.m_filename)))
			log_warn("Failed to add file %s to JSON array", file_stat.m_filename);

		// Free allocated memory
		free(ptr);
	}

	// Close ZIP archive
	mz_zip_reader_end(&zip);

	// Everything worked well
	return NULL;
}

bool free_teleporter_zip(mz_zip_archive *zip)
{
	return mz_zip_writer_end(zip);
}

bool write_teleporter_zip_to_disk(void)
{
	// Generate in-memory ZIP file
	mz_zip_archive zip = { 0 };
	void *ptr = NULL;
	size_t size = 0u;
	char filename[128] = "";
	const char *error = generate_teleporter_zip(&zip, filename, &ptr, &size);
	if(error != NULL)
	{
		log_err("Failed to create Teleporter ZIP file: %s", error);
		return false;
	}

	// Write file to disk
	FILE *fp = fopen(filename, "w");
	if(fp == NULL)
	{
		log_err("Failed to open %s for writing: %s", filename, strerror(errno));
		free_teleporter_zip(&zip);
		free(ptr);
		return false;
	}
	if(fwrite(ptr, 1, size, fp) != size)
	{
		log_err("Failed to write %zu bytes to %s: %s", size, filename, strerror(errno));
		free_teleporter_zip(&zip);
		free(ptr);
		fclose(fp);
		return false;
	}
	fclose(fp);

	// Free allocated ZIP memory, including the archive buffer that
	// mz_zip_writer_finalize_heap_archive() handed over to us
	free_teleporter_zip(&zip);
	free(ptr);

	/* Output filename on successful creation */
	log_info("%s", filename);

	return true;
}

#define MAX_TELEPORTER_ZIP_SIZE (size_t)(128*1024*1024) // 128 MiB

bool read_teleporter_zip_from_disk(const char *filename)
{
	// Open ZIP archive
	FILE *fp = fopen(filename, "r");
	if(fp == NULL)
	{
		log_err("Failed to open %s for reading: %s",
		        filename, strerror(errno));
		return false;
	}

	// Get ZIP archive size
	fseek(fp, 0, SEEK_END);
	const size_t size = (size_t)ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if(size == 0 || size > MAX_TELEPORTER_ZIP_SIZE)
	{
		log_err("ZIP archive %s has an invalid size (%zu bytes, max. %zu bytes)",
		        filename, size, MAX_TELEPORTER_ZIP_SIZE);
		fclose(fp);
		return false;
	}

	// Read ZIP archive to memory
	void *ptr = calloc(size, sizeof(char));
	if(ptr == NULL)
	{
		log_err("Failed to allocate %zu bytes for ZIP archive", size);
		fclose(fp);
		return false;
	}
	if(fread(ptr, 1, size, fp) != size)
	{
		log_err("Failed to read %zu bytes from %s: %s",
		        size, filename, strerror(errno));
		fclose(fp);
		free(ptr);
		return false;
	}
	fclose(fp);

	// Process ZIP archive
	char hint[ERRBUF_SIZE] = "";
	cJSON *imported_files = cJSON_CreateArray();
	if(imported_files == NULL)
	{
		log_err("Failed to create JSON array for imported files");
		free(ptr);
		return false;
	}
	const char *error = read_teleporter_zip(ptr, size, hint, NULL, imported_files);

	if(error != NULL)
	{
		log_err("Failed to read Teleporter ZIP file: %s", error);
		log_err("Hint: %s", hint);
		cJSON_Delete(imported_files);
		free(ptr);
		return false;
	}

	// Output imported files
	for(cJSON *file = imported_files->child; file != NULL; file = file->next)
		log_info("Imported %s", file->valuestring);

	return true;
}
