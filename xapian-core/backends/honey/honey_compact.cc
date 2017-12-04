/** @file glass_compact.cc
 * @brief Compact a glass database, or merge and compact several.
 */
/* Copyright (C) 2004,2005,2006,2007,2008,2009,2010,2011,2012,2013,2014,2015 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include "xapian/compactor.h"
#include "xapian/constants.h"
#include "xapian/error.h"
#include "xapian/types.h"

#include "autoptr.h"
#include <algorithm>
#include <iostream>
#include <queue>

#include <cstdio>
#include <sys/uio.h>

#include "safeerrno.h"

#include "backends/flint_lock.h"
#include "compression_stream.h"
#include "glass_database.h"
#include "glass_defs.h"
#include "glass_table.h"
#include "glass_cursor.h"
#include "glass_version.h"
#include "filetests.h"
#include "internaltypes.h"
#include "pack.h"
#include "backends/valuestats.h"

#include "../byte_length_strings.h"
#include "../prefix_compressed_strings.h"

using namespace std;

// Put all the helpers in a namespace to avoid symbols colliding with those of
// the same name in other flint-derived backends.
namespace GlassCompact {

static inline bool
is_user_metadata_key(const string & key)
{
    return key.size() > 1 && key[0] == '\0' && key[1] == '\xc0';
}

static inline bool
is_valuestats_key(const string & key)
{
    return key.size() > 1 && key[0] == '\0' && key[1] == '\xd0';
}

static inline bool
is_valuechunk_key(const string & key)
{
    return key.size() > 1 && key[0] == '\0' && key[1] == '\xd8';
}

static inline bool
is_doclenchunk_key(const string & key)
{
    return key.size() > 1 && key[0] == '\0' && key[1] == '\xe0';
}

class BufferedFile {
    int fd = -1;
    bool read_only = true;
    mutable size_t buf_end = 0;
    mutable char buf[4096];

  public:
    BufferedFile() { }

    ~BufferedFile() {
	if (fd >= 0) close(fd);
    }

    bool open(const std::string& path, bool read_only_) {
	if (fd >= 0) close(fd);
	read_only = read_only_;
	if (read_only) {
	    // FIXME: add new io_open_stream_rd() etc?
	    fd = io_open_block_rd(path);
	} else {
	    fd = io_open_block_wr(path, true); // FIXME: Always create anew for now...
	}
	return fd >= 0;
    }

    off_t get_pos() const {
	return read_only ?
	    lseek(fd, 0, SEEK_CUR) - buf_end :
	    lseek(fd, 0, SEEK_CUR) + buf_end;
    }

    bool empty() const {
	if (buf_end) return false;
	struct stat sbuf;
	if (fd == -1 || fstat(fd, &sbuf) < 0) return true;
	return (sbuf.st_size == 0);
    }

    void write(unsigned char ch) {
	if (buf_end == sizeof(buf)) {
	    // writev()?
	    if (::write(fd, buf, buf_end)) {
		// FIXME: retry short write
	    }
	    buf_end = 0;
	}
	buf[buf_end++] = ch;
    }

    void write(const char* p, size_t len) {
	if (buf_end + len <= sizeof(buf)) {
	    memcpy(buf + buf_end, p, len);
	    buf_end += len;
	    return;
	}

	while (true) {
	    struct iovec iov[2];
	    iov[0].iov_base = buf;
	    iov[0].iov_len = buf_end;
	    iov[1].iov_base = const_cast<char*>(p);
	    iov[1].iov_len = len;
	    ssize_t n_ = writev(fd, iov, 2);
	    if (n_ < 0) abort();
	    size_t n = n_;
	    if (n == buf_end + len) {
		// Wrote everything.
		buf_end = 0;
		return;
	    }
	    if (n >= buf_end) {
		// Wrote all of buf.
		n -= buf_end;
		p += n;
		len -= n;
		if (::write(fd, p, len)) {
		    // FIXME: retry short write
		}
		buf_end = 0;
		return;
	    }
	    buf_end -= n;
	    memmove(buf, buf + n, buf_end);
	}
    }

    int read() const {
	if (buf_end == 0) {
	    ssize_t r = ::read(fd, buf, sizeof(buf));
	    if (r == 0) return EOF;
	    // FIXME: retry on EINTR
	    if (r < 0) abort();
	    buf_end = r;
	}
	return static_cast<unsigned char>(buf[sizeof(buf) - buf_end--]);
    }

    bool read(char* p, size_t len) const {
	if (len <= buf_end) {
	    memcpy(p, buf + sizeof(buf) - buf_end, len);
	    buf_end -= len;
	    return true;
	}
	memcpy(p, buf + sizeof(buf) - buf_end, buf_end);
	p += buf_end;
	len -= buf_end;
	buf_end = 0;
	// FIXME: refill buffer if len < sizeof(buf)
	return ::read(fd, p, len) == ssize_t(len);
    }

    void flush() {
	if (!read_only && buf_end) {
	    if (::write(fd, buf, buf_end)) {
		// FIXME: retry short write
	    }
	    buf_end = 0;
	}
    }

    void sync() {
	io_sync(fd);
    }

    void rewind() {
	read_only = true;
	lseek(fd, 0, SEEK_SET);
	buf_end = 0;
    }
};

static size_t total_index_size = 0;

class SSIndex {
    std::string data;
    size_t block = 0;
    size_t n_index = 0;
    std::string last_index_key;
    // Put an index entry every this much:
    // FIXME: tune - seems 64K is common elsewhere
    enum { INDEXBLOCK = 1024 };
    SSIndex* parent_index = NULL;

  public:
    SSIndex() { }

    void maybe_add_entry(const std::string& key, off_t ptr) {
	size_t cur_block = ptr / INDEXBLOCK;
	if (cur_block == block)
	    return;

	size_t len = std::min(last_index_key.size(), key.size());
	size_t reuse;
	for (reuse = 0; reuse < len; ++reuse) {
	    if (last_index_key[reuse] != key[reuse]) break;
	}

	data += char(reuse);
	data += char(key.size() - reuse);
	data.append(key, reuse, key.size() - reuse);
	pack_uint(data, static_cast<UNSIGNED_OFF_T>(ptr));

	block = cur_block;
	last_index_key = key;

	++n_index;

	// FIXME: deal with parent_index...

	// FIXME: constant width entries would allow binary chop, but take a
	// lot more space.  could impose max key width and just insert based on
	// that, but still more space than storing key by length.  Or "SKO" -
	// fixed width entry which encodes variable length pointer and key with
	// short keys in the entry and long keys pointed to (or prefix included
	// and rest pointed to).
    }

    off_t write(BufferedFile& fh) {
	off_t root = fh.get_pos();
	fh.write(data.data(), data.size());
	// FIXME: parent stuff...
	return root;
    }

    size_t size() const {
	size_t s = data.size();
	if (parent_index) s += parent_index->size();
	return s;
    }

    size_t get_num_entries() const { return n_index; }
};

class SSTable {
    std::string path;
    bool read_only;
    int flags;
    uint4 compress_min;
    mutable BufferedFile fh;
    mutable std::string last_key;
    SSIndex index;
    off_t root = -1;
    glass_tablesize_t num_entries;
    bool lazy;

  public:
    SSTable(const char*, const std::string& path_, bool read_only_, bool lazy_ = false)
	: path(path_ + GLASS_TABLE_EXTENSION),
	  read_only(read_only_),
	  num_entries(0),
	  lazy(lazy_)
    {
    }

    ~SSTable() {
	size_t index_size = index.size();
	total_index_size += index_size;
	cout << "*** " << path << " - index " << index_size << " for " << index.get_num_entries() << " entries; total_size = " << total_index_size << endl;
    }

    void set_full_compaction(bool) { }

    void set_max_item_size(unsigned) { }

    void create_and_open(int flags_, const RootInfo& root_info) {
	flags = flags_;
	compress_min = root_info.get_compress_min();
	if (read_only) {
	    num_entries = root_info.get_num_entries();
	    root = root_info.get_root();
	    // FIXME: levels
	}
	if (!fh.open(path, read_only))
	    throw Xapian::DatabaseOpeningError("Failed to open SSTable", errno);
    }

    const std::string& get_path() const { return path; }

    void add(const std::string& key, const std::string& val, bool compressed = false) {
	if (read_only)
	    throw Xapian::InvalidOperationError("add() on read-only SSTable");
	if (key.size() == 0 || key.size() > 255)
	    throw Xapian::InvalidArgumentError("Invalid key size: " + str(key.size()));
	if (key <= last_key)
	    throw Xapian::InvalidOperationError("New key <= previous key");
	if (!last_key.empty()) {
	    size_t len = std::min(last_key.size(), key.size());
	    size_t i;
	    for (i = 0; i < len; ++i) {
		if (last_key[i] != key[i]) break;
	    }
	    fh.write(static_cast<unsigned char>(i));
	    fh.write(static_cast<unsigned char>(key.size() - i));
	    fh.write(key.data() + i, key.size() - i);
	} else {
	    fh.write(static_cast<unsigned char>(key.size()));
	    fh.write(key.data(), key.size());
	}
	++num_entries;
	index.maybe_add_entry(key, fh.get_pos());

	// Encode "compressed?" flag in bottom bit.
	// FIXME: Don't do this if a table is uncompressed?  That saves a byte
	// for each item where the extra bit pushes the length up by a byte.
	size_t val_size_enc = (val.size() << 1) | compressed;
	std::string val_len;
	pack_uint(val_len, val_size_enc);
	// FIXME: pass together so we can potentially writev() both?
	fh.write(val_len.data(), val_len.size());
	fh.write(val.data(), val.size());
	last_key = key;
    }

    void flush_db() {
	root = index.write(fh);
	fh.flush();
    }

    void commit(glass_revision_number_t, RootInfo* root_info) {
	if (root < 0)
	    throw Xapian::InvalidOperationError("root not set");

	root_info->set_level(1); // FIXME: number of index levels
	root_info->set_num_entries(num_entries);
	root_info->set_root_is_fake(false);
	// Not really meaningful.
	root_info->set_sequential(true);
	root_info->set_root(root);
	// Not really meaningful.
	root_info->set_blocksize(2048);
	// Not really meaningful.
	//root_info->set_free_list(std::string());

	read_only = true;
	fh.rewind();
	last_key = string();
    }

    void sync() {
	fh.sync();
    }

    bool empty() const {
	return fh.empty();
    }

    bool read_item(std::string& key, std::string& val, bool& compressed) const {
	if (!read_only) {
	    return false;
	}

	int ch = fh.read();
	if (ch == EOF) return false;

	size_t reuse = 0;
	if (!last_key.empty()) {
	    reuse = ch;
	    ch = fh.read();
	    if (ch == EOF) throw Xapian::DatabaseError("EOF/error while reading key length", errno);
	}
	size_t key_size = ch;
	char buf[4096];
	if (!fh.read(buf, key_size))
	    throw Xapian::DatabaseError("read of " + str(key_size) + " bytes of key data failed", errno);
	key.assign(last_key, 0, reuse);
	key.append(buf, key_size);
	last_key = key;

	int r;
	{
	    // FIXME: rework to take advantage of buffering that's happening anyway?
	    char * p = buf;
	    for (int i = 0; i < 8; ++i) {
		int ch2 = fh.read();
		if (ch2 == EOF) {
		    break;
		}
		*p++ = ch2;
		if (ch2 < 128) break;
	    }
	    r = p - buf;
	}
	const char* p = buf;
	const char* end = p + r;
	size_t val_size;
	if (!unpack_uint(&p, end, &val_size)) {
	    throw Xapian::DatabaseError("val_size unpack_uint invalid");
	}
	compressed = val_size & 1;
	val_size >>= 1;
	val.assign(p, end);
	val_size -= (end - p);
	while (val_size) {
	    size_t n = min(val_size, sizeof(buf));
	    if (!fh.read(buf, n))
		throw Xapian::DatabaseError("read of " + str(n) + "/" + str(val_size) + " bytes of value data failed " + str(r), errno);
	    val.append(buf, n);
	    val_size -= n;
	}

	return true;
    }
};

template<typename T> class PostlistCursor;

template<>
class PostlistCursor<GlassTable&> : private GlassCursor {
    Xapian::docid offset;

  public:
    string key, tag;
    Xapian::docid firstdid;
    Xapian::termcount tf, cf;

    PostlistCursor(GlassTable *in, Xapian::docid offset_)
	: GlassCursor(in), offset(offset_), firstdid(0)
    {
	find_entry(string());
	next();
    }

    bool next() {
	if (!GlassCursor::next()) return false;
	// We put all chunks into the non-initial chunk form here, then fix up
	// the first chunk for each term in the merged database as we merge.
	read_tag();
	key = current_key;
	tag = current_tag;
	tf = cf = 0;
	if (is_user_metadata_key(key)) return true;
	if (is_valuestats_key(key)) return true;
	if (is_valuechunk_key(key)) {
	    const char * p = key.data();
	    const char * end = p + key.length();
	    p += 2;
	    Xapian::valueno slot;
	    if (!unpack_uint(&p, end, &slot))
		throw Xapian::DatabaseCorruptError("bad value key");
	    Xapian::docid did;
	    if (!unpack_uint_preserving_sort(&p, end, &did))
		throw Xapian::DatabaseCorruptError("bad value key");
	    did += offset;

	    key.assign("\0\xd8", 2);
	    pack_uint(key, slot);
	    pack_uint_preserving_sort(key, did);
	    return true;
	}

	// Adjust key if this is *NOT* an initial chunk.
	// key is: pack_string_preserving_sort(key, tname)
	// plus optionally: pack_uint_preserving_sort(key, did)
	const char * d = key.data();
	const char * e = d + key.size();
	if (is_doclenchunk_key(key)) {
	    d += 2;
	} else {
	    string tname;
	    if (!unpack_string_preserving_sort(&d, e, tname))
		throw Xapian::DatabaseCorruptError("Bad postlist key");
	}

	if (d == e) {
	    // This is an initial chunk for a term, so adjust tag header.
	    d = tag.data();
	    e = d + tag.size();
	    if (!unpack_uint(&d, e, &tf) ||
		!unpack_uint(&d, e, &cf) ||
		!unpack_uint(&d, e, &firstdid)) {
		throw Xapian::DatabaseCorruptError("Bad postlist key");
	    }
	    ++firstdid;
	    tag.erase(0, d - tag.data());
	} else {
	    // Not an initial chunk, so adjust key.
	    size_t tmp = d - key.data();
	    if (!unpack_uint_preserving_sort(&d, e, &firstdid) || d != e)
		throw Xapian::DatabaseCorruptError("Bad postlist key");
	    if (is_doclenchunk_key(key)) {
		key.erase(tmp);
	    } else {
		key.erase(tmp - 1);
	    }
	}
	firstdid += offset;
	return true;
    }
};

template<>
class PostlistCursor<SSTable&> {
    SSTable* table;
    Xapian::docid offset;

  public:
    string key, tag;
    Xapian::docid firstdid;
    Xapian::termcount tf, cf;

    PostlistCursor(SSTable *in, Xapian::docid offset_)
	: table(in), offset(offset_), firstdid(0)
    {
	next();
    }

    bool next() {
	bool compressed;
	if (!table->read_item(key, tag, compressed))
	    return false;
	if (compressed) abort();
	// We put all chunks into the non-initial chunk form here, then fix up
	// the first chunk for each term in the merged database as we merge.
	tf = cf = 0;
	if (is_user_metadata_key(key)) return true;
	if (is_valuestats_key(key)) return true;
	if (is_valuechunk_key(key)) {
	    const char * p = key.data();
	    const char * end = p + key.length();
	    p += 2;
	    Xapian::valueno slot;
	    if (!unpack_uint(&p, end, &slot))
		throw Xapian::DatabaseCorruptError("bad value key");
	    Xapian::docid did;
	    if (!unpack_uint_preserving_sort(&p, end, &did))
		throw Xapian::DatabaseCorruptError("bad value key");
	    did += offset;

	    key.assign("\0\xd8", 2);
	    pack_uint(key, slot);
	    pack_uint_preserving_sort(key, did);
	    return true;
	}

	// Adjust key if this is *NOT* an initial chunk.
	// key is: pack_string_preserving_sort(key, tname)
	// plus optionally: pack_uint_preserving_sort(key, did)
	const char * d = key.data();
	const char * e = d + key.size();
	if (is_doclenchunk_key(key)) {
	    d += 2;
	} else {
	    string tname;
	    if (!unpack_string_preserving_sort(&d, e, tname))
		throw Xapian::DatabaseCorruptError("Bad postlist key");
	}

	if (d == e) {
	    // This is an initial chunk for a term, so adjust tag header.
	    d = tag.data();
	    e = d + tag.size();
	    if (!unpack_uint(&d, e, &tf) ||
		!unpack_uint(&d, e, &cf) ||
		!unpack_uint(&d, e, &firstdid)) {
		throw Xapian::DatabaseCorruptError("Bad postlist key");
	    }
	    ++firstdid;
	    tag.erase(0, d - tag.data());
	} else {
	    // Not an initial chunk, so adjust key.
	    size_t tmp = d - key.data();
	    if (!unpack_uint_preserving_sort(&d, e, &firstdid) || d != e)
		throw Xapian::DatabaseCorruptError("Bad postlist key");
	    if (is_doclenchunk_key(key)) {
		key.erase(tmp);
	    } else {
		key.erase(tmp - 1);
	    }
	}
	firstdid += offset;
	return true;
    }
};

template<typename T>
class PostlistCursorGt {
  public:
    /** Return true if and only if a's key is strictly greater than b's key.
     */
    bool operator()(const T* a, const T* b) const {
	if (a->key > b->key) return true;
	if (a->key != b->key) return false;
	return (a->firstdid > b->firstdid);
    }
};

static string
encode_valuestats(Xapian::doccount freq,
		  const string & lbound, const string & ubound)
{
    string value;
    pack_uint(value, freq);
    pack_string(value, lbound);
    // We don't store or count empty values, so neither of the bounds
    // can be empty.  So we can safely store an empty upper bound when
    // the bounds are equal.
    if (lbound != ubound) value += ubound;
    return value;
}

// U : vector<GlassTable*>::const_iterator
template<typename T, typename U> void
merge_postlists(Xapian::Compactor * compactor,
		T * out, vector<Xapian::docid>::const_iterator offset,
		U b, U e)
{
    typedef decltype(**b) table_type; // E.g. GlassTable
    typedef PostlistCursor<table_type> cursor_type;
    typedef PostlistCursorGt<cursor_type> gt_type;
    priority_queue<cursor_type *, vector<cursor_type *>, gt_type> pq;
    for ( ; b != e; ++b, ++offset) {
	auto in = *b;
	if (in->empty()) {
	    // Skip empty tables.
	    continue;
	}

	pq.push(new cursor_type(in, *offset));
    }

    string last_key;
    {
	// Merge user metadata.
	vector<string> tags;
	while (!pq.empty()) {
	    cursor_type * cur = pq.top();
	    const string& key = cur->key;
	    if (!is_user_metadata_key(key)) break;

	    if (key != last_key) {
		if (!tags.empty()) {
		    if (tags.size() > 1 && compactor) {
			Assert(!last_key.empty());
			// FIXME: It would be better to merge all duplicates
			// for a key in one call, but currently we don't in
			// multipass mode.
			const string & resolved_tag =
			    compactor->resolve_duplicate_metadata(last_key,
								  tags.size(),
								  &tags[0]);
			out->add(last_key, resolved_tag);
		    } else {
			Assert(!last_key.empty());
			out->add(last_key, tags[0]);
		    }
		    tags.resize(0);
		}
		last_key = key;
	    }
	    tags.push_back(cur->tag);

	    pq.pop();
	    if (cur->next()) {
		pq.push(cur);
	    } else {
		delete cur;
	    }
	}
	if (!tags.empty()) {
	    if (tags.size() > 1 && compactor) {
		Assert(!last_key.empty());
		const string & resolved_tag =
		    compactor->resolve_duplicate_metadata(last_key,
							  tags.size(),
							  &tags[0]);
		out->add(last_key, resolved_tag);
	    } else {
		Assert(!last_key.empty());
		out->add(last_key, tags[0]);
	    }
	}
    }

    {
	// Merge valuestats.
	Xapian::doccount freq = 0;
	string lbound, ubound;

	while (!pq.empty()) {
	    cursor_type * cur = pq.top();
	    const string& key = cur->key;
	    if (!is_valuestats_key(key)) break;
	    if (key != last_key) {
		// For the first valuestats key, last_key will be the previous
		// key we wrote, which we don't want to overwrite.  This is the
		// only time that freq will be 0, so check that.
		if (freq) {
		    out->add(last_key, encode_valuestats(freq, lbound, ubound));
		    freq = 0;
		}
		last_key = key;
	    }

	    const string & tag = cur->tag;

	    const char * pos = tag.data();
	    const char * end = pos + tag.size();

	    Xapian::doccount f;
	    string l, u;
	    if (!unpack_uint(&pos, end, &f)) {
		if (*pos == 0) throw Xapian::DatabaseCorruptError("Incomplete stats item in value table");
		throw Xapian::RangeError("Frequency statistic in value table is too large");
	    }
	    if (!unpack_string(&pos, end, l)) {
		if (*pos == 0) throw Xapian::DatabaseCorruptError("Incomplete stats item in value table");
		throw Xapian::RangeError("Lower bound in value table is too large");
	    }
	    size_t len = end - pos;
	    if (len == 0) {
		u = l;
	    } else {
		u.assign(pos, len);
	    }
	    if (freq == 0) {
		freq = f;
		lbound = l;
		ubound = u;
	    } else {
		freq += f;
		if (l < lbound) lbound = l;
		if (u > ubound) ubound = u;
	    }

	    pq.pop();
	    if (cur->next()) {
		pq.push(cur);
	    } else {
		delete cur;
	    }
	}

	if (freq) {
	    out->add(last_key, encode_valuestats(freq, lbound, ubound));
	}
    }

    // Merge valuestream chunks.
    while (!pq.empty()) {
	cursor_type * cur = pq.top();
	const string & key = cur->key;
	if (!is_valuechunk_key(key)) break;
	Assert(!is_user_metadata_key(key));
	out->add(key, cur->tag);
	pq.pop();
	if (cur->next()) {
	    pq.push(cur);
	} else {
	    delete cur;
	}
    }

    Xapian::termcount tf = 0, cf = 0; // Initialise to avoid warnings.
    vector<pair<Xapian::docid, string> > tags;
    while (true) {
	cursor_type * cur = NULL;
	if (!pq.empty()) {
	    cur = pq.top();
	    pq.pop();
	}
	Assert(cur == NULL || !is_user_metadata_key(cur->key));
	if (cur == NULL || cur->key != last_key) {
	    if (!tags.empty()) {
		string first_tag;
		pack_uint(first_tag, tf);
		pack_uint(first_tag, cf);
		pack_uint(first_tag, tags[0].first - 1);
		string tag = tags[0].second;
		tag[0] = (tags.size() == 1) ? '1' : '0';
		first_tag += tag;
		out->add(last_key, first_tag);

		string term;
		if (!is_doclenchunk_key(last_key)) {
		    const char * p = last_key.data();
		    const char * end = p + last_key.size();
		    if (!unpack_string_preserving_sort(&p, end, term) || p != end)
			throw Xapian::DatabaseCorruptError("Bad postlist chunk key");
		}

		vector<pair<Xapian::docid, string> >::const_iterator i;
		i = tags.begin();
		while (++i != tags.end()) {
		    tag = i->second;
		    tag[0] = (i + 1 == tags.end()) ? '1' : '0';
		    out->add(pack_glass_postlist_key(term, i->first), tag);
		}
	    }
	    tags.clear();
	    if (cur == NULL) break;
	    tf = cf = 0;
	    last_key = cur->key;
	}
	tf += cur->tf;
	cf += cur->cf;
	tags.push_back(make_pair(cur->firstdid, cur->tag));
	if (cur->next()) {
	    pq.push(cur);
	} else {
	    delete cur;
	}
    }
}

template<typename T> struct MergeCursor;

template<>
struct MergeCursor<GlassTable&> : public GlassCursor {
    explicit MergeCursor(GlassTable *in) : GlassCursor(in) {
	find_entry(string());
	next();
    }
};

template<>
struct MergeCursor<SSTable&> {
    SSTable* table;
    string current_key, current_tag;
    bool current_compressed;
    mutable CompressionStream comp_stream;

    explicit MergeCursor(SSTable *in) : table(in), comp_stream(Z_DEFAULT_STRATEGY) {
	next();
    }

    bool next() {
	return table->read_item(current_key, current_tag, current_compressed);
    }

    bool read_tag(bool keep_compressed) {
	if (!keep_compressed && current_compressed) {
	    // Need to decompress.
	    comp_stream.decompress_start();
	    string new_tag;
	    if (!comp_stream.decompress_chunk(current_tag.data(), current_tag.size(), new_tag)) {
		// Decompression didn't complete.
		abort();
	    }
	    swap(current_tag, new_tag);
	    current_compressed = false;
	}
	return current_compressed;
    }
};

template<typename T>
struct CursorGt {
    /// Return true if and only if a's key is strictly greater than b's key.
    bool operator()(const T* a, const T* b) const {
	if (b->after_end()) return false;
	if (a->after_end()) return true;
	return (a->current_key > b->current_key);
    }
};

// U : vector<GlassTable*>::const_iterator
template<typename T, typename U> void
merge_spellings(T* out, U b, U e)
{
    typedef decltype(**b) table_type; // E.g. GlassTable
    typedef MergeCursor<table_type> cursor_type;
    typedef CursorGt<cursor_type> gt_type;
    priority_queue<cursor_type *, vector<cursor_type *>, gt_type> pq;
    for ( ; b != e; ++b) {
	auto in = *b;
	if (!in->empty()) {
	    pq.push(new cursor_type(in));
	}
    }

    while (!pq.empty()) {
	cursor_type * cur = pq.top();
	pq.pop();

	string key = cur->current_key;
	if (pq.empty() || pq.top()->current_key > key) {
	    // No need to merge the tags, just copy the (possibly compressed)
	    // tag value.
	    bool compressed = cur->read_tag(true);
	    out->add(key, cur->current_tag, compressed);
	    if (cur->next()) {
		pq.push(cur);
	    } else {
		delete cur;
	    }
	    continue;
	}

	// Merge tag values with the same key:
	string tag;
	if (key[0] != 'W') {
	    // We just want the union of words, so copy over the first instance
	    // and skip any identical ones.
	    priority_queue<PrefixCompressedStringItor *,
			   vector<PrefixCompressedStringItor *>,
			   PrefixCompressedStringItorGt> pqtag;
	    // Stick all the cursor_type pointers in a vector because their
	    // current_tag members must remain valid while we're merging their
	    // tags, but we need to call next() on them all afterwards.
	    vector<cursor_type *> vec;
	    vec.reserve(pq.size());

	    while (true) {
		cur->read_tag();
		pqtag.push(new PrefixCompressedStringItor(cur->current_tag));
		vec.push_back(cur);
		if (pq.empty() || pq.top()->current_key != key) break;
		cur = pq.top();
		pq.pop();
	    }

	    PrefixCompressedStringWriter wr(tag);
	    string lastword;
	    while (!pqtag.empty()) {
		PrefixCompressedStringItor * it = pqtag.top();
		pqtag.pop();
		string word = **it;
		if (word != lastword) {
		    lastword = word;
		    wr.append(lastword);
		}
		++*it;
		if (!it->at_end()) {
		    pqtag.push(it);
		} else {
		    delete it;
		}
	    }

	    for (auto i : vec) {
		cur = i;
		if (cur->next()) {
		    pq.push(cur);
		} else {
		    delete cur;
		}
	    }
	} else {
	    // We want to sum the frequencies from tags for the same key.
	    Xapian::termcount tot_freq = 0;
	    while (true) {
		cur->read_tag();
		Xapian::termcount freq;
		const char * p = cur->current_tag.data();
		const char * end = p + cur->current_tag.size();
		if (!unpack_uint_last(&p, end, &freq) || freq == 0) {
		    throw Xapian::DatabaseCorruptError("Bad spelling word freq");
		}
		tot_freq += freq;
		if (cur->next()) {
		    pq.push(cur);
		} else {
		    delete cur;
		}
		if (pq.empty() || pq.top()->current_key != key) break;
		cur = pq.top();
		pq.pop();
	    }
	    tag.resize(0);
	    pack_uint_last(tag, tot_freq);
	}
	out->add(key, tag);
    }
}

// U : vector<GlassTable*>::const_iterator
template<typename T, typename U> void
merge_synonyms(T* out, U b, U e)
{
    typedef decltype(**b) table_type; // E.g. GlassTable
    typedef MergeCursor<table_type> cursor_type;
    typedef CursorGt<cursor_type> gt_type;
    priority_queue<cursor_type *, vector<cursor_type *>, gt_type> pq;
    for ( ; b != e; ++b) {
	auto in = *b;
	if (!in->empty()) {
	    pq.push(new cursor_type(in));
	}
    }

    while (!pq.empty()) {
	cursor_type * cur = pq.top();
	pq.pop();

	string key = cur->current_key;
	if (pq.empty() || pq.top()->current_key > key) {
	    // No need to merge the tags, just copy the (possibly compressed)
	    // tag value.
	    bool compressed = cur->read_tag(true);
	    out->add(key, cur->current_tag, compressed);
	    if (cur->next()) {
		pq.push(cur);
	    } else {
		delete cur;
	    }
	    continue;
	}

	// Merge tag values with the same key:
	string tag;

	// We just want the union of words, so copy over the first instance
	// and skip any identical ones.
	priority_queue<ByteLengthPrefixedStringItor *,
		       vector<ByteLengthPrefixedStringItor *>,
		       ByteLengthPrefixedStringItorGt> pqtag;
	vector<cursor_type *> vec;

	while (true) {
	    cur->read_tag();
	    pqtag.push(new ByteLengthPrefixedStringItor(cur->current_tag));
	    vec.push_back(cur);
	    if (pq.empty() || pq.top()->current_key != key) break;
	    cur = pq.top();
	    pq.pop();
	}

	string lastword;
	while (!pqtag.empty()) {
	    ByteLengthPrefixedStringItor * it = pqtag.top();
	    pqtag.pop();
	    if (**it != lastword) {
		lastword = **it;
		tag += byte(lastword.size() ^ MAGIC_XOR_VALUE);
		tag += lastword;
	    }
	    ++*it;
	    if (!it->at_end()) {
		pqtag.push(it);
	    } else {
		delete it;
	    }
	}

	for (auto i : vec) {
	    cur = i;
	    if (cur->next()) {
		pq.push(cur);
	    } else {
		delete cur;
	    }
	}

	out->add(key, tag);
    }
}

template<typename T, typename U> void
multimerge_postlists(Xapian::Compactor * compactor,
		     T* out, const char * tmpdir,
		     const vector<U*>& in,
		     vector<Xapian::docid> off)
{
    if (in.size() <= 3) {
	merge_postlists(compactor, out, off.begin(), in.begin(), in.end());
	return;
    }
    unsigned int c = 0;
    vector<SSTable *> tmp;
    tmp.reserve(in.size() / 2);
    {
	vector<Xapian::docid> newoff;
	newoff.resize(in.size() / 2);
	for (unsigned int i = 0, j; i < in.size(); i = j) {
	    j = i + 2;
	    if (j == in.size() - 1) ++j;

	    string dest = tmpdir;
	    char buf[64];
	    sprintf(buf, "/tmp%u_%u.", c, i / 2);
	    dest += buf;

	    SSTable * tmptab = new SSTable("postlist", dest, false);

	    // Use maximum blocksize for temporary tables.  And don't compress
	    // entries in temporary tables, even if the final table would do
	    // so.  Any already compressed entries will get copied in
	    // compressed form. (FIXME: SSTable has no blocksize)
	    RootInfo root_info;
	    root_info.init(65536, 0);
	    const int flags = Xapian::DB_DANGEROUS|Xapian::DB_NO_SYNC;
	    tmptab->create_and_open(flags, root_info);

	    merge_postlists(compactor, tmptab, off.begin() + i,
			    in.begin() + i, in.begin() + j);
	    tmp.push_back(tmptab);
	    tmptab->flush_db();
	    tmptab->commit(1, &root_info);
	    AssertRel(root_info.get_blocksize(),==,65536);
	}
	swap(off, newoff);
	++c;
    }

    while (tmp.size() > 3) {
	vector<SSTable *> tmpout;
	tmpout.reserve(tmp.size() / 2);
	vector<Xapian::docid> newoff;
	newoff.resize(tmp.size() / 2);
	for (unsigned int i = 0, j; i < tmp.size(); i = j) {
	    j = i + 2;
	    if (j == tmp.size() - 1) ++j;

	    string dest = tmpdir;
	    char buf[64];
	    sprintf(buf, "/tmp%u_%u.", c, i / 2);
	    dest += buf;

	    SSTable * tmptab = new SSTable("postlist", dest, false);

	    // Use maximum blocksize for temporary tables.  And don't compress
	    // entries in temporary tables, even if the final table would do
	    // so.  Any already compressed entries will get copied in
	    // compressed form. (FIXME: SSTable has no blocksize)
	    RootInfo root_info;
	    root_info.init(65536, 0);
	    const int flags = Xapian::DB_DANGEROUS|Xapian::DB_NO_SYNC;
	    tmptab->create_and_open(flags, root_info);

	    merge_postlists(compactor, tmptab, off.begin() + i,
			    tmp.begin() + i, tmp.begin() + j);
	    if (c > 0) {
		for (unsigned int k = i; k < j; ++k) {
		    // FIXME: unlink(tmp[k]->get_path().c_str());
		    delete tmp[k];
		    tmp[k] = NULL;
		}
	    }
	    tmpout.push_back(tmptab);
	    tmptab->flush_db();
	    tmptab->commit(1, &root_info);
	    AssertRel(root_info.get_blocksize(),==,65536);
	}
	swap(tmp, tmpout);
	swap(off, newoff);
	++c;
    }
    merge_postlists(compactor, out, off.begin(), tmp.begin(), tmp.end());
    if (c > 0) {
	for (size_t k = 0; k < tmp.size(); ++k) {
	    // FIXME: unlink(tmp[k]->get_path().c_str());
	    delete tmp[k];
	    tmp[k] = NULL;
	}
    }
}

template<typename T> class PositionCursor;

template<>
class PositionCursor<GlassTable&> : private GlassCursor {
    Xapian::docid offset;

  public:
    string key;
    Xapian::docid firstdid;

    PositionCursor(GlassTable *in, Xapian::docid offset_)
	: GlassCursor(in), offset(offset_), firstdid(0) {
	find_entry(string());
	next();
    }

    bool next() {
	if (!GlassCursor::next()) return false;
	read_tag();
	const char * d = current_key.data();
	const char * e = d + current_key.size();
	string term;
	Xapian::docid did;
	if (!unpack_string_preserving_sort(&d, e, term) ||
	    !unpack_uint_preserving_sort(&d, e, &did) ||
	    d != e) {
	    throw Xapian::DatabaseCorruptError("Bad position key");
	}

	key.resize(0);
	pack_string_preserving_sort(key, term);
	pack_uint_preserving_sort(key, did + offset);
	return true;
    }

    const string & get_tag() const {
	return current_tag;
    }
};

template<>
class PositionCursor<SSTable&> {
    SSTable* table;
    Xapian::docid offset;

  public:
    string key;
    string current_key, current_tag;
    Xapian::docid firstdid;

    PositionCursor(SSTable *in, Xapian::docid offset_)
	: table(in), offset(offset_), firstdid(0) {
	next();
    }

    bool next() {
	bool compressed;
	if (!table->read_item(current_key, current_tag, compressed))
	    return false;
	if (compressed) abort();
	const char * d = current_key.data();
	const char * e = d + current_key.size();
	string term;
	Xapian::docid did;
	if (!unpack_string_preserving_sort(&d, e, term) ||
	    !unpack_uint_preserving_sort(&d, e, &did) ||
	    d != e) {
	    throw Xapian::DatabaseCorruptError("Bad position key");
	}

	key.resize(0);
	pack_string_preserving_sort(key, term);
	pack_uint_preserving_sort(key, did + offset);
	return true;
    }

    const string & get_tag() const {
	return current_tag;
    }
};

template<typename T>
class PositionCursorGt {
  public:
    /** Return true if and only if a's key is strictly greater than b's key.
     */
    bool operator()(const T* a, const T* b) const {
	return a->key > b->key;
    }
};

template<typename T, typename U> void
merge_positions(T* out, const vector<U*> & inputs,
		const vector<Xapian::docid> & offset)
{
    typedef decltype(*inputs[0]) table_type; // E.g. GlassTable
    typedef PositionCursor<table_type> cursor_type;
    typedef PositionCursorGt<cursor_type> gt_type;
    priority_queue<cursor_type *, vector<cursor_type *>, gt_type> pq;
    for (size_t i = 0; i < inputs.size(); ++i) {
	auto in = inputs[i];
	if (in->empty()) {
	    // Skip empty tables.
	    continue;
	}

	pq.push(new cursor_type(in, offset[i]));
    }

    while (!pq.empty()) {
	cursor_type * cur = pq.top();
	pq.pop();
	out->add(cur->key, cur->get_tag());
	if (cur->next()) {
	    pq.push(cur);
	} else {
	    delete cur;
	}
    }
}

template<typename T, typename U> void
merge_docid_keyed(T *out, const vector<U*> & inputs,
		  const vector<Xapian::docid> & offset)
{
    for (size_t i = 0; i < inputs.size(); ++i) {
	Xapian::docid off = offset[i];

	auto in = inputs[i];
	if (in->empty()) continue;

	GlassCursor cur(in);
	cur.find_entry(string());

	string key;
	while (cur.next()) {
	    // Adjust the key if this isn't the first database.
	    if (off) {
		Xapian::docid did;
		const char * d = cur.current_key.data();
		const char * e = d + cur.current_key.size();
		if (!unpack_uint_preserving_sort(&d, e, &did)) {
		    string msg = "Bad key in ";
		    msg += inputs[i]->get_path();
		    throw Xapian::DatabaseCorruptError(msg);
		}
		did += off;
		key.resize(0);
		pack_uint_preserving_sort(key, did);
		if (d != e) {
		    // Copy over the termname for the position table.
		    key.append(d, e - d);
		}
	    } else {
		key = cur.current_key;
	    }
	    bool compressed = cur.read_tag(true);
	    out->add(key, cur.current_tag, compressed);
	}
    }
}

}

using namespace GlassCompact;

void
GlassDatabase::compact(Xapian::Compactor * compactor,
		       const char * destdir,
		       int fd,
		       const vector<Xapian::Database::Internal*> & sources,
		       const vector<Xapian::docid> & offset,
		       size_t block_size,
		       Xapian::Compactor::compaction_level compaction,
		       unsigned flags,
		       Xapian::docid last_docid)
{
    struct table_list {
	// The "base name" of the table.
	const char * name;
	// The type.
	Glass::table_type type;
	// Create tables after position lazily.
	bool lazy;
    };

    static const table_list tables[] = {
	// name		type			lazy
	{ "postlist",	Glass::POSTLIST,	false },
	{ "docdata",	Glass::DOCDATA,		true },
	{ "termlist",	Glass::TERMLIST,	false },
	{ "position",	Glass::POSITION,	true },
	{ "spelling",	Glass::SPELLING,	true },
	{ "synonym",	Glass::SYNONYM,		true }
    };
    const table_list * tables_end = tables +
	(sizeof(tables) / sizeof(tables[0]));

    const int FLAGS = Xapian::DB_DANGEROUS;

    bool single_file = (flags & Xapian::DBCOMPACT_SINGLE_FILE);
    bool multipass = (flags & Xapian::DBCOMPACT_MULTIPASS);
    if (single_file) {
	// FIXME: Support this combination - we need to put temporary files
	// somewhere.
	multipass = false;
    }

    if (single_file) {
	for (size_t i = 0; i != sources.size(); ++i) {
	    GlassDatabase * db = static_cast<GlassDatabase*>(sources[i]);
	    if (db->has_uncommitted_changes()) {
		const char * m =
		    "Can't compact from a WritableDatabase with uncommitted "
		    "changes - either call commit() first, or create a new "
		    "Database object from the filename on disk";
		throw Xapian::InvalidOperationError(m);
	    }
	}
    }

    if (block_size < GLASS_MIN_BLOCKSIZE ||
	block_size > GLASS_MAX_BLOCKSIZE ||
	(block_size & (block_size - 1)) != 0) {
	block_size = GLASS_DEFAULT_BLOCKSIZE;
    }

    FlintLock lock(destdir ? destdir : "");
    if (!single_file) {
	string explanation;
	FlintLock::reason why = lock.lock(true, false, explanation);
	if (why != FlintLock::SUCCESS) {
	    lock.throw_databaselockerror(why, destdir, explanation);
	}
    }

    AutoPtr<GlassVersion> version_file_out;
    if (single_file) {
	if (destdir) {
	    fd = open(destdir, O_RDWR|O_CREAT|O_BINARY|O_CLOEXEC, 0666);
	    if (fd < 0) {
		throw Xapian::DatabaseCreateError("open() failed", errno);
	    }
	}
	version_file_out.reset(new GlassVersion(fd));
    } else {
	fd = -1;
	version_file_out.reset(new GlassVersion(destdir));
    }

    version_file_out->create(block_size);
    for (size_t i = 0; i != sources.size(); ++i) {
	GlassDatabase * db = static_cast<GlassDatabase*>(sources[i]);
	version_file_out->merge_stats(db->version_file);
    }

    string fl_serialised;
#if 0
    if (single_file) {
	GlassFreeList fl;
	fl.set_first_unused_block(1); // FIXME: Assumption?
	fl.pack(fl_serialised);
    }
#endif

    vector<SSTable *> tabs;
    tabs.reserve(tables_end - tables);
    off_t prev_size = block_size;
    for (const table_list * t = tables; t < tables_end; ++t) {
	// The postlist table requires an N-way merge, adjusting the
	// headers of various blocks.  The spelling and synonym tables also
	// need special handling.  The other tables have keys sorted in
	// docid order, so we can merge them by simply copying all the keys
	// from each source table in turn.
	if (compactor)
	    compactor->set_status(t->name, string());

	string dest;
	if (!single_file) {
	    dest = destdir;
	    dest += '/';
	    dest += t->name;
	    dest += '.';
	}

	bool output_will_exist = !t->lazy;

	// Sometimes stat can fail for benign reasons (e.g. >= 2GB file
	// on certain systems).
	bool bad_stat = false;

	// We can't currently report input sizes if there's a single file DB
	// amongst the inputs.
	bool single_file_in = false;

	off_t in_size = 0;

	vector<GlassTable*> inputs;
	inputs.reserve(sources.size());
	size_t inputs_present = 0;
	for (auto src : sources) {
	    GlassDatabase * db = static_cast<GlassDatabase*>(src);
	    GlassTable * table;
	    switch (t->type) {
		case Glass::POSTLIST:
		    table = &(db->postlist_table);
		    break;
		case Glass::DOCDATA:
		    table = &(db->docdata_table);
		    break;
		case Glass::TERMLIST:
		    table = &(db->termlist_table);
		    break;
		case Glass::POSITION:
		    table = &(db->position_table);
		    break;
		case Glass::SPELLING:
		    table = &(db->spelling_table);
		    break;
		case Glass::SYNONYM:
		    table = &(db->synonym_table);
		    break;
		default:
		    Assert(false);
		    return;
	    }

	    if (db->single_file()) {
		if (t->lazy && table->empty()) {
		    // Essentially doesn't exist.
		} else {
		    // FIXME: Find actual size somehow?
		    // in_size += table->size() / 1024;
		    single_file_in = true;
		    output_will_exist = true;
		    ++inputs_present;
		}
	    } else {
		off_t db_size = file_size(table->get_path());
		if (errno == 0) {
		    in_size += db_size / 1024;
		    output_will_exist = true;
		    ++inputs_present;
		} else if (errno != ENOENT) {
		    // We get ENOENT for an optional table.
		    bad_stat = true;
		    output_will_exist = true;
		    ++inputs_present;
		}
	    }
	    inputs.push_back(table);
	}

	// If any inputs lack a termlist table, suppress it in the output.
	if (t->type == Glass::TERMLIST && inputs_present != sources.size()) {
	    if (inputs_present != 0) {
		if (compactor) {
		    string m = str(inputs_present);
		    m += " of ";
		    m += str(sources.size());
		    m += " inputs present, so suppressing output";
		    compactor->set_status(t->name, m);
		}
		continue;
	    }
	    output_will_exist = false;
	}

	if (!output_will_exist) {
	    if (compactor)
		compactor->set_status(t->name, "doesn't exist");
	    continue;
	}

	SSTable * out;
	if (single_file) {
//	    out = new GlassTable(t->name, fd, version_file_out->get_offset(),
//				 false, false);
	    out = NULL;
	} else {
	    out = new SSTable(t->name, dest, false, t->lazy);
	}
	tabs.push_back(out);
	RootInfo * root_info = version_file_out->root_to_set(t->type);
	if (single_file) {
	    root_info->set_free_list(fl_serialised);
//	    out->open(FLAGS, version_file_out->get_root(t->type), version_file_out->get_revision());
	} else {
	    out->create_and_open(FLAGS, *root_info);
	}

	out->set_full_compaction(compaction != compactor->STANDARD);
	if (compaction == compactor->FULLER) out->set_max_item_size(1);

	switch (t->type) {
	    case Glass::POSTLIST: {
		if (multipass && inputs.size() > 3) {
		    multimerge_postlists(compactor, out, destdir,
					 inputs, offset);
		} else {
		    merge_postlists(compactor, out, offset.begin(),
				    inputs.begin(), inputs.end());
		}
		break;
	    }
	    case Glass::SPELLING:
		merge_spellings(out, inputs.begin(), inputs.end());
		break;
	    case Glass::SYNONYM:
		merge_synonyms(out, inputs.begin(), inputs.end());
		break;
	    case Glass::POSITION:
		merge_positions(out, inputs, offset);
		break;
	    default:
		// DocData, Termlist
		merge_docid_keyed(out, inputs, offset);
		break;
	}

	// Commit as revision 1.
	out->flush_db();
	out->commit(1, root_info);
	out->sync();
	if (single_file) fl_serialised = root_info->get_free_list();

	off_t out_size = 0;
	if (!bad_stat && !single_file_in) {
	    off_t db_size;
	    if (single_file) {
		db_size = file_size(fd);
	    } else {
		db_size = file_size(dest + GLASS_TABLE_EXTENSION);
	    }
	    if (errno == 0) {
		if (single_file) {
		    off_t old_prev_size = max(prev_size, off_t(block_size));
		    prev_size = db_size;
		    db_size -= old_prev_size;
		}
		out_size = db_size / 1024;
	    } else {
		bad_stat = (errno != ENOENT);
	    }
	}
	if (bad_stat) {
	    if (compactor)
		compactor->set_status(t->name, "Done (couldn't stat all the DB files)");
	} else if (single_file_in) {
	    if (compactor)
		compactor->set_status(t->name, "Done (table sizes unknown for single file DB input)");
	} else {
	    string status;
	    if (out_size == in_size) {
		status = "Size unchanged (";
	    } else {
		off_t delta;
		if (out_size < in_size) {
		    delta = in_size - out_size;
		    status = "Reduced by ";
		} else {
		    delta = out_size - in_size;
		    status = "INCREASED by ";
		}
		if (in_size) {
		    status += str(100 * delta / in_size);
		    status += "% ";
		}
		status += str(delta);
		status += "K (";
		status += str(in_size);
		status += "K -> ";
	    }
	    status += str(out_size);
	    status += "K)";
	    if (compactor)
		compactor->set_status(t->name, status);
	}
    }

    // If compacting to a single file output and all the tables are empty, pad
    // the output so that it isn't mistaken for a stub database when we try to
    // open it.  For this it needs to be a multiple of 2KB in size.
    if (single_file && prev_size < off_t(block_size)) {
#ifdef HAVE_FTRUNCATE
	if (ftruncate(fd, block_size) < 0) {
	    throw Xapian::DatabaseError("Failed to set size of output database", errno);
	}
#else
	const off_t off = block_size - 1;
	if (lseek(fd, off, SEEK_SET) != off || write(fd, "", 1) != 1) {
	    throw Xapian::DatabaseError("Failed to set size of output database", errno);
	}
#endif
    }

    if (single_file) {
	if (lseek(fd, version_file_out->get_offset(), SEEK_SET) == -1) {
	    throw Xapian::DatabaseError("lseek() failed", errno);
	}
    }
    version_file_out->set_last_docid(last_docid);
    string tmpfile = version_file_out->write(1, FLAGS);
    for (unsigned j = 0; j != tabs.size(); ++j) {
	tabs[j]->sync();
    }
    // Commit with revision 1.
    version_file_out->sync(tmpfile, 1, FLAGS);
    for (unsigned j = 0; j != tabs.size(); ++j) {
	delete tabs[j];
    }

    if (!single_file) lock.release();
}