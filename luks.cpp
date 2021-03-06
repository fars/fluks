/* Copyright (c) 2009-2011, Markus Peloquin <markus@cs.wisc.edu>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS' AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/timer.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <openssl/rand.h>

#include "af.hpp"
#include "cipher.hpp"
#include "crypt.hpp"
#include "detect.hpp"
#include "gutmann.hpp"
#include "hash.hpp"
#include "luks.hpp"
#include "pbkdf2.hpp"
#include "os.hpp"
#include "support.hpp"

namespace fluks {
namespace {

const uint16_t	PBKDF2_BENCH_ITER = 10000;

std::string	make_mode(const std::string &, const std::string &,
		    const std::string &);
bool		parse_cipher(const std::string &, std::string &,
		    std::string &, std::string &, std::string &);

// reconstruct the mode string (e.g. 'cbc', 'cbc-essiv', 'cbc-essiv:sha256')
std::string
make_mode(const std::string &block_mode, const std::string &ivmode,
    const std::string &ivhash)
{
	std::ostringstream out;
	if (block_mode.size()) {
		out << block_mode;
		if (ivmode.size()) {
			out << '-' << ivmode;
			if (ivhash.size())
				out << ':' << ivhash;
		}
	}
	return out.str();
}


// TODO use parse_cipher_spec() instead
bool
parse_cipher(const std::string &cipher_spec, std::string &cipher,
	    std::string &block_mode, std::string &ivmode, std::string &ivhash)
{
	// valid patterns:
	// [^-]* - [^-*]
	// [^-]* - [^-*] - [^:]*
	// [^-]* - [^-*] - [^:]* : .*
	boost::regex expr(
	    "([^-]+) - ([^-]+)  (?: - ([^:]+) )?  (?: : (.+) )?",
	    boost::regex_constants::normal |
	    boost::regex_constants::mod_x); // ignore space

	boost::smatch matches;
	if (!boost::regex_match(cipher_spec, matches, expr))
		return false;

	cipher = matches[1];
	block_mode = matches[2];
	ivmode = matches[3];
	ivhash = matches[4];
	return true;
}

void dump(const std::string &pfx, const uint8_t *buf, size_t sz)
{
	char oldfill = std::cout.fill('0');
	std::cout << std::hex;

	std::cout << pfx << '\n';
	for (size_t i = 0; i < sz; i++)
		std::cout << std::setw(2) << (short)buf[i];
	std::cout << '\n';

	std::cout.fill(oldfill);
	std::cout << std::dec;
}

void dump_hash(const std::string &pfx, const uint8_t *buf, size_t sz)
{
	std::shared_ptr<Hash_function> hash = Hash_function::create(HT_SHA1);
	hash->init();
	hash->add(buf, sz);
	uint8_t out[hash->traits()->digest_size];
	hash->end(out);

	dump(pfx, out, sizeof(out));
}

} // end anon namespace
}

bool
fluks::check_magic(const struct phdr1 *header)
{
	return std::equal(MAGIC, MAGIC + sizeof(MAGIC), header->magic);
}

bool
fluks::check_version_1(const struct phdr1 *header)
{
	return header->version == 1;
}

fluks::Luks_header::Luks_header(std::shared_ptr<std::sys_fstream> device,
    int32_t sz_key, const std::string &cipher_spec,
    const std::string &hash_spec, uint32_t mk_iterations, uint32_t stripes)
    throw (boost::system::system_error, Bad_spec) :
	_device(device),
	_hdr(new struct phdr1),
	_master_key(0),
	_sz_sect(sector_size(*device)),
	_hash_type(Hash_traits::type(hash_spec)),
	_proved_passwd(-1),
	_mach_end(true),
	_dirty(true),
	_key_need_erase(NUM_KEYS, false)
{
	init_cipher_spec(cipher_spec, sz_key);
	_master_key.reset(new uint8_t[_hdr->sz_key]);

	if (_hash_type == HT_UNDEFINED)
		throw Bad_spec("unrecognized hash");

	// initialize LUKS header

#ifdef DEBUG
	// for valgrind
	std::fill(_master_key.get(), _master_key.get() + _hdr->sz_key, 0);
#endif
	if (!RAND_bytes(_master_key.get(), _hdr->sz_key))
		throw Ssl_error();

	std::copy(MAGIC, MAGIC + sizeof(MAGIC),_hdr->magic);
	_hdr->version = 1;

	// write the canonized hash name into the header
	{
		std::string hash = Hash_traits::traits(_hash_type)->name;
		std::copy(hash.begin(), hash.end(), _hdr->hash_spec);
		_hdr->hash_spec[hash.size()] = '\0';
	}

#ifdef DEBUG
	// for valgrind
	std::fill(_hdr->mk_salt, _hdr->mk_salt + SZ_SALT, 0);
#endif
	if (!RAND_bytes(_hdr->mk_salt, SZ_SALT))
		throw Ssl_error();
	_hdr->mk_iterations = mk_iterations;

	// hash the master key
	pbkdf2(_hash_type, _master_key.get(), _hdr->sz_key,
	    _hdr->mk_salt, SZ_SALT, _hdr->mk_iterations,
	    _hdr->mk_digest, SZ_MK_DIGEST);

	// LUKS defines off_base as
	//	floor(sizeof(phdr) / sz_sect) + 1,
	// but it is clearly more correct to use
	//	ceil(sizeof(phdr) / sz_sect).
	// the same goes for km_sectors
	uint32_t off_base = (sizeof(phdr1) + _sz_sect - 1) / _sz_sect;
	uint32_t km_sectors =
	    (stripes * _hdr->sz_key + _sz_sect - 1) / _sz_sect;

	for (uint8_t i = 0; i < NUM_KEYS; i++) {
		_hdr->keys[i].active = KEY_DISABLED;
		_hdr->keys[i].iterations = 0;
		_hdr->keys[i].stripes = stripes;
		_hdr->keys[i].off_km = off_base;

		off_base += km_sectors;
	}

	_hdr->off_payload = off_base;

	// generates a warning with >=gcc-4.6 and <boost-1.49
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	std::string uuid_str = boost::lexical_cast<std::string>(uuid);
	std::copy(uuid_str.begin(), uuid_str.end(), _hdr->uuid);
}

fluks::Luks_header::Luks_header(std::shared_ptr<std::sys_fstream> device)
    throw (boost::system::system_error, Bad_spec, Disk_error, No_header,
    Unsupported_version) :
	_device(device),
	_hdr(new struct phdr1),
	_sz_sect(sector_size(*device)),
	_proved_passwd(-1),
	_mach_end(false),
	_dirty(false),
	_key_need_erase(NUM_KEYS, false)
{
	if (!_device->seekg(0, std::ios_base::beg))
		throw Disk_error("failed to seek to header");
	if (!_device->read(reinterpret_cast<char *>(_hdr.get()),
	    sizeof(struct phdr1)))
		throw Disk_error("failed to read header");

	// big-endian -> machine-endian
	set_mach_end(true);

	if (!check_magic(_hdr.get()))
		throw No_header();

	if (!check_version_1(_hdr.get()))
		throw Unsupported_version();

	_hash_type = Hash_traits::type(_hdr->hash_spec);

	if (_hash_type == HT_UNDEFINED)
		throw Bad_spec(
		    std::string("undefined hash spec in header: ") +
		    _hdr->hash_spec);

	// recreate the cipher-spec string
	std::string cipher_spec = _hdr->cipher_name;
	if (*_hdr->cipher_mode) {
		cipher_spec += '-';
		cipher_spec += _hdr->cipher_mode;
	}
	init_cipher_spec(cipher_spec, _hdr->sz_key);
}

bool
fluks::Luks_header::read_key(const std::string &passwd, int8_t hint)
    throw (Disk_error)
{
	if (_master_key)
		return false;

	set_mach_end(true);

	uint8_t master_key[_hdr->sz_key];
	uint8_t key_digest[SZ_MK_DIGEST];
	uint8_t i;
	uint8_t max;

	if (static_cast<uint16_t>(hint) >= NUM_KEYS) hint = -1;

	if (hint >= 0) {
		i = hint;
		max = hint + 1;
	} else {
		i = 0;
		max = NUM_KEYS;
	}

	// find a slot than can be decrypted with the password, copy the
	// data to _master_key
	for (; i < max; i++) {
		if (_hdr->keys[i].active == KEY_DISABLED) continue;
		decrypt_key(passwd, i, key_digest, master_key);

		if (std::equal(key_digest, key_digest + sizeof(key_digest),
		    _hdr->mk_digest)) {
			_proved_passwd = i;
			_master_key.reset(new uint8_t[_hdr->sz_key]);
			std::copy(master_key, master_key + sizeof(master_key),
			    _master_key.get());
			return true;
		}
	}
	return false;
}

void
fluks::Luks_header::add_passwd(const std::string &passwd, uint32_t check_time)
    throw (No_private_key, Slots_full)
{
	struct key	*avail = 0;
	uint8_t		avail_idx = 0;

	if (!_master_key) throw No_private_key();

	set_mach_end(true);

	// find an open slot
	for (uint8_t i = 0; i < NUM_KEYS; i++) {
		if (_hdr->keys[i].active == KEY_DISABLED) {
			avail = _hdr->keys + i;
			avail_idx = i;
			break;
		}
	}
	if (!avail) throw Slots_full();

	uint8_t split_key[_hdr->sz_key * avail->stripes];

#ifdef DEBUG
	// for valgrind
	std::fill(avail->salt, avail->salt + SZ_SALT, 0);
#endif
	if (!RAND_bytes(avail->salt, SZ_SALT))
		throw Ssl_error();
	af_split(_master_key.get(), _hdr->sz_key, avail->stripes,
	    _hash_type, split_key);

	uint8_t pw_digest[_hdr->sz_key];

	// benchmark the PBKDF2 function
	boost::timer timer;
	pbkdf2(_hash_type,
	    reinterpret_cast<const uint8_t *>(passwd.c_str()), passwd.size(),
	    avail->salt, SZ_SALT, PBKDF2_BENCH_ITER,
	    pw_digest, sizeof(pw_digest));
	// timer.elapsed() gives seconds
	avail->iterations = static_cast<uint32_t>(
	    PBKDF2_BENCH_ITER * check_time / (timer.elapsed() * 1000000));

	// compute digest for realsies
	pbkdf2(_hash_type,
	    reinterpret_cast<const uint8_t *>(passwd.c_str()), passwd.size(),
	    avail->salt, SZ_SALT, avail->iterations,
	    pw_digest, sizeof(pw_digest));

	// encrypt the master key with pw_digest
	std::shared_ptr<Crypter> crypter = Crypter::create(pw_digest,
	    sizeof(pw_digest), *_cipher_spec);
	_key_crypt[avail_idx].reset(new uint8_t[sizeof(split_key)]);
	crypter->encrypt(avail->off_km, _sz_sect,
	    split_key, sizeof(split_key), _key_crypt[avail_idx].get());

	// verify that decryption works just as well
	uint8_t crypt_check[sizeof(split_key)];
	crypter->decrypt(avail->off_km, _sz_sect,
	    _key_crypt[avail_idx].get(), sizeof(split_key), crypt_check);

	Assert(std::equal(crypt_check, crypt_check + sizeof(crypt_check),
	    split_key), "ciphertext couldn't be decrypted");

	avail->active = KEY_ENABLED;
	_dirty = true;
}

bool
fluks::Luks_header::check_supported(std::ostream *out, uint16_t max_version)
{
	uint16_t	vers;
	bool		good = true;

	vers = Cipher_traits::traits(_cipher_spec->type_cipher())->
	    luks_version;
	if (!vers || (max_version && vers > max_version)) {
		if (!out) return false;
		good = false;
		*out << "WARNING: using cipher not in LUKS spec.\n";
	}
	vers = block_mode_info::version(_cipher_spec->type_block_mode());
	if (!vers || (max_version && vers > max_version)) {
		if (!out) return false;
		good = false;
		*out << "WARNING: using block mode not in LUKS spec.\n";
	}
	vers = iv_mode_info::version(_cipher_spec->type_iv_mode());
	if (!vers || (max_version && vers > max_version)) {
		if (!out) return false;
		good = false;
		*out << "WARNING: using IV mode not in LUKS spec.\n";
	}
	vers = Hash_traits::traits(_cipher_spec->type_iv_hash())->
	    luks_version;
	if (!vers || (max_version && vers > max_version)) {
		if (!out) return false;
		good = false;
		*out << "WARNING: using IV hash not in LUKS spec.\n";
	}
	vers = Hash_traits::traits(_hash_type)->luks_version;
	if (!vers || (max_version && vers > max_version)) {
		if (!out) return false;
		good = false;
		*out << "WARNING: using hash not in LUKS spec.\n";
	}
	if (!good)
		*out << "WARNING: these specs will not work with other LUKS "
		    "implementations.\n";

	return good;
}

std::string
fluks::Luks_header::info() const
{
	const_cast<Luks_header *>(this)->set_mach_end(true);
	std::ostringstream out;

	out <<   "version                             " << _hdr->version
	    << "\ncipher                              " << _hdr->cipher_name
	    << "\ncipher mode                         " << _hdr->cipher_mode
	    << "\nhash spec                           " << _hdr->hash_spec
	    << "\npayload start sector                " << _hdr->off_payload
	    << "\nmaster key size                     " << _hdr->sz_key
	    << "\nmaster key iterations               " << _hdr->mk_iterations
	    << "\nuuid                                " << _hdr->uuid;
	for (uint16_t i = 0; i < NUM_KEYS; i++) {
		out << "\nkey " << i << " state                         "
		    << (_hdr->keys[i].active == KEY_ENABLED ?
		    "ENABLED" : "DISABLED")
		    << "\nkey " << i << " iterations                    "
		    << _hdr->keys[i].iterations
		    << "\nkey " << i << " key material sector offset    "
		    << _hdr->keys[i].off_km
		    << "\nkey " << i << " stripes                       "
		    << _hdr->keys[i].stripes;
	}
	return out.str();
}

void
fluks::Luks_header::revoke_slot(uint8_t which) throw (Safety)
{
	if (!_master_key)
		throw Safety("will not allow a revokation while the "
		    "master key is unknown");
	if (which == _proved_passwd)
		throw Safety("only the passwords not used to decrypt the "
		    "master key are allowed to be revoked");

	set_mach_end(true);

	_hdr->keys[which].active = KEY_DISABLED;
	_dirty = true;
	_key_need_erase[which] = true;
}

void
fluks::Luks_header::wipe() throw (Disk_error, Safety)
{
	if (!_master_key)
		throw Safety("will not allow the header to be wiped while "
		    "the master key is unknown");

	gutmann_erase(*_device, 0, _hdr->off_payload);
}

void
fluks::Luks_header::save() throw (Disk_error)
{
	if (!_dirty) return;

	set_mach_end(true);

	// first erase old keys and then commit new keys
	for (uint8_t i = 0; i < NUM_KEYS; i++) {
		if (_key_need_erase[i]) {
			gutmann_erase(*_device,
			    _hdr->keys[i].off_km * _sz_sect,
			    _hdr->sz_key * _hdr->keys[i].stripes);
			_key_need_erase[i] = false;
		}

		if (_key_crypt[i]) {
			if (!_device->seekp(_hdr->keys[i].off_km * _sz_sect,
			    std::ios_base::beg))
				throw Disk_error("writing key "
				    "material: seek error");

			if (!_device->write(reinterpret_cast<char *>(
			    _key_crypt[i].get()),
			    _hdr->sz_key * _hdr->keys[i].stripes))
				throw Disk_error("writing key "
				    "material: write error");
			_key_crypt[i].reset();
		}
	}

	if (_dirty) {
		// ensure big-endian
		set_mach_end(false);

		if (!_device->seekp(0, std::ios_base::beg))
			throw Disk_error("writing header: seek error");

		if (!_device->write(reinterpret_cast<char *>(_hdr.get()),
		    sizeof(struct phdr1)))
			throw Disk_error("writing header: write error");

		_dirty = false;
	}

	// run dmsetup
	// NAME = device-mapper name
	// LOGICAL_START_SECTOR
	// dmsetup create NAME --table "LOGICAL_START_SECTOR NUM_SECTORS crypt CIPHER KEY IV_OFFSET DEVICE_PATH OFFSET"
}

// initializes the values of the cipher-spec enums, the cipher-spec
// strings in the LUKS header, and the sz_key value in the LUKS header,
// throwing Bad_spec as necessary
void
fluks::Luks_header::init_cipher_spec(const std::string &cipher_spec,
    int32_t sz_key) throw (Bad_spec)
{
	set_mach_end(true);

	// parse and check
	_cipher_spec.reset(new Cipher_spec(sz_key, cipher_spec));

	if (sz_key == -1) {
		// use the largest possible size
		const Cipher_traits *traits =
		    Cipher_traits::traits(_cipher_spec->type_cipher());
		const std::vector<uint16_t> &sizes = traits->key_sizes;
		sz_key = sizes.back();
	}
	_hdr->sz_key = sz_key;

	// recreate a canonical cipher spec
	std::string cipher = _cipher_spec->canon_cipher();
	std::string mode = _cipher_spec->canon_mode();

	// copy specs (back) into header
	std::copy(cipher.begin(), cipher.end(), _hdr->cipher_name);
	_hdr->cipher_name[cipher.size()] = '\0';
	std::copy(mode.begin(), mode.end(), _hdr->cipher_mode);
	_hdr->cipher_mode[mode.size()] = '\0';
}

int8_t
fluks::Luks_header::locate_passwd(const std::string &passwd) throw (Disk_error)
{
	set_mach_end(true);

	uint8_t key_digest[SZ_MK_DIGEST];
	uint8_t master_key[_hdr->sz_key];

	// find the first slot that can be decrypted with the password
	for (uint8_t i = 0; i < NUM_KEYS; i++) {
		if (_hdr->keys[i].active == KEY_DISABLED) continue;
		decrypt_key(passwd, i, key_digest, master_key);

		if (std::equal(key_digest, key_digest + sizeof(key_digest),
		    _hdr->mk_digest))
			return i;
	}
	return -1;
}

// key_digest should be as large as the digest size of the hash
// master_key should be as large as _hdr->sz_key
void
fluks::Luks_header::decrypt_key(const std::string &passwd, uint8_t slot,
    uint8_t key_digest[SZ_MK_DIGEST], uint8_t *master_key)
{
	set_mach_end(true);

	uint8_t pw_digest[_hdr->sz_key];
	struct key *key = _hdr->keys + slot;
	uint8_t key_crypt[_hdr->sz_key * key->stripes];
	uint8_t split_key[sizeof(key_crypt)];

	// password => pw_digest
	pbkdf2(_hash_type,
	    reinterpret_cast<const uint8_t *>(passwd.c_str()), passwd.size(),
	    key->salt, SZ_SALT, key->iterations,
	    pw_digest, sizeof(pw_digest));

	// disk => key_crypt
	if (!_device->seekg(key->off_km * _sz_sect, std::ios_base::beg))
		throw Disk_error("failed to seek to key material");

	if (!_device->read(reinterpret_cast<char *>(key_crypt),
	    sizeof(key_crypt)))
		throw Disk_error("failed to read key material");

	// (pw_digest, key_crypt) => split_key
	{
		std::shared_ptr<Crypter> crypter = Crypter::create(pw_digest,
		    sizeof(pw_digest), *_cipher_spec);
		crypter->decrypt(key->off_km, _sz_sect,
		    key_crypt, sizeof(key_crypt), split_key);
	}

	// split_key => master_key
	af_merge(split_key, _hdr->sz_key, key->stripes,
	    _hash_type, master_key);

	// master_key => key_digest
	pbkdf2(_hash_type,
	    master_key, _hdr->sz_key,
	    _hdr->mk_salt, SZ_SALT, _hdr->mk_iterations,
	    key_digest, SZ_MK_DIGEST);
}
