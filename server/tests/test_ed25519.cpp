// SHA-512 and Ed25519 against vectors made elsewhere, as in test_password.cpp.
//
// SHA-512 against FIPS 180-4's examples and Python's hashlib; signatures
// against RFC 8032 section 7.1 and OpenSSL 3.0 (random seeds, messages from
// message_of_length; tools/ed25519_compare.cpp checks as many more as wanted).
// The refusals were built with affine curve arithmetic written separately in
// Python, each to get past a verifier that skips one of the checks. OpenSSL
// 3.0.13 takes every forgery below that uses a key of small order, the ones
// with a key written as y + p included; it refuses S + L.
#include "../src/util/ed25519.h"
#include "../src/util/password.h"
#include "../src/util/sha512.h"
#include "test_util.h"

#include <string>
#include <vector>

using fernsdr::ed25519_public_key;
using fernsdr::ed25519_sign;
using fernsdr::ed25519_verify;
using fernsdr::to_hex;

namespace {

std::vector<uint8_t> bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    CHECK(fernsdr::from_hex(hex, out));
    return out;
}

std::string sha512_hex(const std::string& input) {
    uint8_t digest[64];
    fernsdr::Sha512 hash;
    hash.update(input);
    hash.finish(digest);
    return to_hex(digest, sizeof(digest));
}

// The messages of the OpenSSL vectors, so the table need not carry them.
std::vector<uint8_t> message_of_length(size_t n) {
    std::vector<uint8_t> message(n);
    for (size_t i = 0; i < n; i++) message[i] = static_cast<uint8_t>(i * 7 + n);
    return message;
}

bool verifies(const std::vector<uint8_t>& public_key, const std::vector<uint8_t>& message,
              const std::vector<uint8_t>& signature) {
    return ed25519_verify(public_key.data(), message.data(), message.size(), signature.data());
}

// The key from the seed, the signature over the message, and that the
// expected signature verifies.
void check_vector(const std::string& seed_hex, const std::string& public_hex, const std::vector<uint8_t>& message,
                  const std::string& signature_hex) {
    const std::vector<uint8_t> seed = bytes(seed_hex);
    uint8_t public_key[32], signature[64];
    ed25519_public_key(seed.data(), public_key);
    CHECK_EQ_STR(to_hex(public_key, 32), public_hex);
    ed25519_sign(seed.data(), message.data(), message.size(), signature);
    CHECK_EQ_STR(to_hex(signature, 64), signature_hex);
    CHECK(verifies(bytes(public_hex), message, bytes(signature_hex)));
}

// RFC 8032, 7.1, TEST 3: the signature the refusals below start from.
const char* const kPublic3 = "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025";
const char* const kSignature3 =
    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
    "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a";

}  // namespace

TEST_CASE(sha512_matches_the_published_vectors) {
    CHECK_EQ_STR(sha512_hex(""),
                 "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                 "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    CHECK_EQ_STR(sha512_hex("abc"),
                 "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                 "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK_EQ_STR(sha512_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                            "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
                 "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                 "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    CHECK_EQ_STR(sha512_hex(std::string(1000000, 'a')),
                 "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                 "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
    // The padding boundaries, from Python's hashlib: 111 is the last length
    // whose padding and 16-byte length fit in the same block, 112 the first
    // that needs another, 128 a whole block, and 239/240 the same one block on.
    CHECK_EQ_STR(sha512_hex(std::string(111, 'x')),
                 "9a2a120825c2319867758ec277924f6faa254968bf752046dacdd948d8ad299b"
                 "10359fd04bfd7d3810b5fa1b16a294236138baff981cbb85248478053ac4d3dd");
    CHECK_EQ_STR(sha512_hex(std::string(112, 'x')),
                 "a3722b515ef40c910f2419f6e0da8ca51d410114ce6272faae64045f9e9f630e"
                 "7fa8dd5a3243c9860b899d148c3da4bc0f9e07454542604d030bb55531fe0d5b");
    CHECK_EQ_STR(sha512_hex(std::string(127, 'x')),
                 "1d5a8893e7b7ed83d485d26f88cfb846f3760279916976fe538e539fc16f7cd1"
                 "9ba3e1c2cd5fda78749a74205755cdf694e8fa90b2bfed8815f406af76c1d7bf");
    CHECK_EQ_STR(sha512_hex(std::string(128, 'x')),
                 "e2e22f8422b54b06e35c3ea30a383d1de7a8fbc27992923074103117020d8dd7"
                 "024c3ecf7d6d1a15a6de5a75ff32fb486b9e8ced4c02ffe05822bf2cb734d0e0");
    CHECK_EQ_STR(sha512_hex(std::string(129, 'x')),
                 "19015483ea99fa74728fd7b13adba6b729cfa4ab7c388573efa2641d2af0577f"
                 "24dd51acbaeec41a7a11d10f6c4e68a3b90e7b2348c678486a4365c9d9101cdd");
    CHECK_EQ_STR(sha512_hex(std::string(239, 'x')),
                 "805c40517b44cef414a06c632b4ee9c21d34c85a84c3f9530274db86e2a3e38c"
                 "b0e54025666b925426bd6860f81538abbfab13011e85767d814a245d232a6044");
    CHECK_EQ_STR(sha512_hex(std::string(240, 'x')),
                 "3e05caa7e0fddab685e6052bd902c316505180504344d00b1217e1349a532d1d"
                 "0e5ce4b24d69eea3aca369dc16d8bbdf872b2fe1770e0e1aad22f84e0519bc6c");
}

TEST_CASE(sha512_is_the_same_however_the_input_is_split) {
    std::string text(1000, '\0');
    for (size_t i = 0; i < text.size(); i++) text[i] = static_cast<char>(i * 31 + 7);
    const std::string whole = sha512_hex(text);
    for (const size_t piece : {1, 3, 111, 127, 128, 129, 999}) {
        fernsdr::Sha512 hash;
        for (size_t at = 0; at < text.size(); at += piece) hash.update(text.substr(at, piece));
        uint8_t digest[64];
        hash.finish(digest);
        CHECK_EQ_STR(to_hex(digest, 64), whole);
    }
}

TEST_CASE(ed25519_matches_rfc_8032) {
    check_vector("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", {},
                 "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                 "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    check_vector("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
                 "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", bytes("72"),
                 "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                 "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    check_vector("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", kPublic3, bytes("af82"),
                 kSignature3);
    // TEST SHA(abc): the message is SHA-512("abc").
    check_vector("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
                 "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
                 bytes(sha512_hex("abc")),
                 "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
                 "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704");
}

// The lengths put the ends of both hashes a signature takes, SHA-512(prefix ||
// M) with 32 bytes before the message and SHA-512(R || A || M) with 64, on
// either side of the padding boundaries (79/80 and 47/48, 175/176), on a whole
// block (96, 64) and across several blocks (1000).
TEST_CASE(ed25519_matches_openssl) {
    struct Vector {
        const char* seed;
        const char* public_key;
        size_t length;
        const char* signature;
    };
    const Vector vectors[] = {
        {"5a889cce15c1d9fd29cdb8c45476bbd97dc9e76212401fd2c4d0f01ce47d507d",
         "7b10a53e4a9ca01ce52c3671e4b1068eb3a2fb630e1bbbc530ad0668cb7f7db9", 1,
         "a6f5b71c52595a698958730785bba31395c17a6c42d4523810706fefbb1238b0"
         "f5b9d2e843e5e08a1a4b7f5f43151556ff3b155c5e274d115e7550e8e7e79c0d"},
        {"c0de068a5a35cf289e09467b36491c9a6a5904018ff4f39be345c64de54122fd",
         "18a914148d015b97f6068b7f201221dc667a9a2ff48bdb1ce9f4aa1e8acdd90e", 47,
         "b31e30806faaa73c77cb9c658888cab376e494b70d1529f9cebf3dc9ff19eda1"
         "2658a8736f7a3b2f65005e10f15afbd3e3d19414c5801df3c67febae8876bf01"},
        {"0fe902dc4413a4a016152b0508ff57773c6d71b33ae26b591e2a8830c7c0fff2",
         "fdfe118f7760f4c16fa1e622a8fda65a5d7c7c7ee824512acef585f85f59744e", 48,
         "2226fb70bc2d1c5fc3950153dc6a787627f3421bd7a27261aaf9049f289046a0"
         "2699b56d566bd191b0baede327810f4aaffb6e9173f9cea83947aabed0dab804"},
        {"316d091fbb99ef1d7fc1f842c567658c2e27b42030087df8a1fcf11258c6a0ab",
         "9a1666e0adf1d96ec7d3b4ebeb27ba5bc9eded775cade1378d67cb87d7659a09", 64,
         "d117c5db7958a96a74f02b9501745993f60aec3f9fddd7a4113acbea0459e6b8"
         "c2644eb02a7df17a33d920cc87e7c47c508fe9d03129b557e2b12ca68b24c605"},
        {"4090cbc85f3c006731431de0b4cd88786f0c41fc4d280a52ca61a33d751eca0b",
         "47ac83c6cbc5b5442a41306462680dbecf82a2407e3453881cc29d9817b785c2", 79,
         "87f08b6111c3bf02c7317a571f4a8a3973cdd5ce45b13575ef9a74cd310218a7"
         "bc954c098d7802c2bdcc0527b6a7203882ba8785acd773e63b7bbfa2f6ac010b"},
        {"c46a6fdc66c7d0778f53bc6e1d7e2e006a21bc0d6dc193110f7bbcb658f0ac73",
         "8dd2b47d1c2fc3e7a1574fce51855a4de972eb117aa4e51c598cbde3770f7b4e", 80,
         "a7943e26942a79262fad31da56aca5453dd5285e4e0e61716263d15d873ed145"
         "51884abd2920db83d67deec48614d2b12a15fde3d4f4fcb2f6316ead2466d90f"},
        {"c81fcc08b5918c050c1318a356d843b34a4fd57c20729c96f1625de8749d7d6b",
         "f5cbaf4bb436a0104503563161be92f0e8f74bf34cd3b90e70d67aa884003b9e", 96,
         "53e3f92b3c829ff0909a886b33c142f07e3e03701f487849545d165cbe3cdea7"
         "a5a2a0b70194d645aa65573b57bb7c0c0333cff4e85830ac421441be1cb25202"},
        {"46c74ce49355c3b8788281f12313b80f702ca9cce3ea40693f84404c0b556739",
         "166b9405325319e715479824ef2354494f64f50b66bc5d0bb882dd346d9b1c64", 175,
         "9bd3d03a9e5e115b0f1d002365ada0e4e5377b985803036283e0c4e92b6b9438"
         "39f716af6e48b288728f22cb99bb0712ee01ee00f01d70780cc2e949be915206"},
        {"8fe1c95569fec80c9e57667b4943b7fad04e553cf33572f5ed8e5f217784d64b",
         "a4d7524d622135cffc553e8735f1e9702d61fdc489ae6b829aea2625c118eee7", 176,
         "cb6278b95ab71e987c171df54762117090419983634cd7094a32114ff86355ea"
         "046ffd96a2404b042f162b0ff47f3d0e60d93c4a6e5c7fe9701ffd05aea4270c"},
        {"32876ff589509d73f0fb645311ece441412632ca8ca37636578145a96b1376e7",
         "45e7bcd4d254c23e3caa6fcdc818e824cd41222aec58edec531e8f72da4b5a0c", 1000,
         "ac657c6be2e24b880022e062d7c8d8ec731f64105d76a70e1cfbaed028299ff6"
         "cdd7a2c325bd8398cbb1333ebccf98378d844fb602c1317c732982efe75f2e0d"},
    };
    for (const Vector& v : vectors) check_vector(v.seed, v.public_key, message_of_length(v.length), v.signature);
}

// One bit at a time, a different one of the eight in each byte.
TEST_CASE(ed25519_refuses_a_changed_message_key_or_signature) {
    const std::vector<uint8_t> key = bytes(kPublic3), message = bytes("af82"), signature = bytes(kSignature3);
    CHECK(verifies(key, message, signature));
    for (size_t i = 0; i < signature.size(); i++) {
        std::vector<uint8_t> changed = signature;
        changed[i] ^= static_cast<uint8_t>(1 << (i % 8));
        CHECK(!verifies(key, message, changed));
    }
    for (size_t i = 0; i < key.size(); i++) {
        std::vector<uint8_t> changed = key;
        changed[i] ^= static_cast<uint8_t>(1 << (i % 8));
        CHECK(!verifies(changed, message, signature));
    }
    for (size_t i = 0; i < message.size(); i++) {
        std::vector<uint8_t> changed = message;
        changed[i] ^= static_cast<uint8_t>(1 << (i % 8));
        CHECK(!verifies(key, changed, signature));
    }
    CHECK(!verifies(key, bytes("af"), signature));
    CHECK(!verifies(key, bytes("af8200"), signature));
}

// [S + L]B is [S]B, so a verifier that took any S would take TEST 3 with L
// added to S: a second signature for the same message, made without the key.
TEST_CASE(ed25519_refuses_an_s_not_below_the_group_order) {
    const std::vector<uint8_t> key = bytes(kPublic3), message = bytes("af82");
    const std::string r = std::string(kSignature3).substr(0, 64);
    CHECK(!verifies(key, message, bytes(r + "05d391b0a77904e98404ef037747a56e"
                                            "4a7c15e9716ed28dc027beceea1ec41a")));
    const std::string order = "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010";
    CHECK(!verifies(key, message, bytes(r + order)));
    CHECK(!verifies(key, message, bytes(r + std::string(64, 'f'))));
}

// With a key A of order 1, 2, 4 or 8, [k]A is one of at most eight points,
// so R = B - [j]A and S = 1 verify whenever the challenge k comes out as j
// modulo that order; each forgery here was found by trying messages. The
// last two write their key's y as y + p, which decoding refuses before the
// order is looked at.
TEST_CASE(ed25519_refuses_keys_of_small_order) {
    struct Forgery {
        const char* key;
        const char* message;
        const char* r;
    };
    const Forgery forgeries[] = {
        {"0100000000000000000000000000000000000000000000000000000000000000", "0100",
         "5866666666666666666666666666666666666666666666666666666666666666"},
        {"ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "0200",
         "9599999999999999999999999999999999999999999999999999999999999999"},
        {"0000000000000000000000000000000000000000000000000000000000000080", "0100",
         "5252cc0a7f208133b620acbd4537eba2a4123bf0a8c2e4f980c3b31bb69765ea"},
        {"c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a", "0100",
         "55ae61520ca466adcc4ae4a32dc1633a5d749c64a5b50f136fc3469f27e487e6"},
        {"eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "0100",
         "5866666666666666666666666666666666666666666666666666666666666666"},
        {"edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "0300",
         "9599999999999999999999999999999999999999999999999999999999999999"},
    };
    const std::string one = "0100000000000000000000000000000000000000000000000000000000000000";
    for (const Forgery& f : forgeries) CHECK(!verifies(bytes(f.key), bytes(f.message), bytes(f.r + one)));
}

// Only the key's owner can make these: R the neutral point and S = k * a,
// with TEST 3's secret scalar a. Written canonically, R verifies; written as
// y + p it has to be refused, as RFC 8032 refuses to decode it, although it
// names the same point.
TEST_CASE(ed25519_refuses_an_r_not_in_canonical_form) {
    const std::vector<uint8_t> key = bytes(kPublic3), message = bytes("af82");
    CHECK(verifies(key, message,
                   bytes("0100000000000000000000000000000000000000000000000000000000000000"
                         "89653588ead4771e2326253c8cc167ed5f891f0c238314c9d136bb6e54f2f206")));
    CHECK(!verifies(key, message,
                    bytes("eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"
                          "620ae1280170f1ff1b92660606e73b8834b78fac04b1f3d6e438a19ae49b220f")));
}

// y = 3 is a point of order 8L, a key nobody knows the secret of but a key;
// written as y + p it names the same point, and decoding refuses it. No
// signature can show that refusal: a key written as y + p has a y under 19,
// and nobody knows the secret of such a point unless it has small order,
// which is refused anyway. Then the keys of small order from above, y = 2,
// and the neutral point with its sign bit set.
TEST_CASE(ed25519_keys_are_valid_only_on_the_curve_in_canonical_form_and_not_of_small_order) {
    const auto valid = [](const std::string& key) { return fernsdr::ed25519_key_is_valid(bytes(key).data()); };
    CHECK(valid(kPublic3));
    CHECK(valid("0300000000000000000000000000000000000000000000000000000000000000"));
    CHECK(!valid("f0ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"));
    for (const char* key : {"0100000000000000000000000000000000000000000000000000000000000000",
                            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                            "0000000000000000000000000000000000000000000000000000000000000080",
                            "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
                            "0200000000000000000000000000000000000000000000000000000000000000",
                            "0100000000000000000000000000000000000000000000000000000000000080"}) {
        CHECK(!valid(key));
    }
}

// y = 2 has no x on the curve, and x = 0 has no negative to mark with the
// sign bit. With TEST 1's signature, whose message is empty.
TEST_CASE(ed25519_refuses_keys_that_are_not_points) {
    const std::vector<uint8_t> signature = bytes(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(!verifies(bytes("0200000000000000000000000000000000000000000000000000000000000000"), {}, signature));
    CHECK(!verifies(bytes("0100000000000000000000000000000000000000000000000000000000000080"), {}, signature));
}
