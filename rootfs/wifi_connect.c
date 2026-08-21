// SPDX-License-Identifier: GPL-2.0-only
/* Dependency-free WPA2-PSK nl80211 association test for ESP32-S31. */

#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define NLA_ALIGNTO 4
#define NLA_ALIGN_LEN(n) (((n) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_DATA_PTR(a) ((void *)((char *)(a) + NLA_HDRLEN))
#define NLA_PAYLOAD_LEN(a) ((int)(a)->nla_len - NLA_HDRLEN)
#define WPA2_VERSION 2
#define CCMP_SUITE 0x000fac04U
#define PSK_SUITE  0x000fac02U
#define SAE_SUITE  0x000fac08U

struct sha1_ctx {
	uint32_t state[5];
	uint64_t bytes;
	uint8_t block[64];
};

static uint32_t sequence;

static uint32_t rol(uint32_t value, unsigned int bits)
{
	return (value << bits) | (value >> (32 - bits));
}

static void sha1_transform(struct sha1_ctx *ctx, const uint8_t block[64])
{
	uint32_t w[80], a, b, c, d, e, f, k, tmp;
	unsigned int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)block[i * 4] << 24 |
		       (uint32_t)block[i * 4 + 1] << 16 |
		       (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
	for (; i < 80; i++)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
	d = ctx->state[3]; e = ctx->state[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (b & c) | (~b & d); k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d; k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d; k = 0xca62c1d6;
		}
		tmp = rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol(b, 30); b = a; a = tmp;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
	ctx->state[3] += d; ctx->state[4] += e;
}

static void sha1_init(struct sha1_ctx *ctx)
{
	ctx->state[0] = 0x67452301; ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe; ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xc3d2e1f0; ctx->bytes = 0;
}

static void sha1_update(struct sha1_ctx *ctx, const void *data, size_t length)
{
	const uint8_t *input = data;
	size_t used = ctx->bytes & 63, take;

	ctx->bytes += length;
	while (length) {
		take = 64 - used;
		if (take > length)
			take = length;
		memcpy(ctx->block + used, input, take);
		used += take; input += take; length -= take;
		if (used == 64) {
			sha1_transform(ctx, ctx->block);
			used = 0;
		}
	}
}

static void sha1_final(struct sha1_ctx *ctx, uint8_t digest[20])
{
	uint64_t bits = ctx->bytes * 8;
	uint8_t pad[72] = { 0x80 };
	size_t used = ctx->bytes & 63, padlen = used < 56 ? 56 - used : 120 - used;
	unsigned int i;

	for (i = 0; i < 8; i++)
		pad[padlen + i] = bits >> (56 - i * 8);
	sha1_update(ctx, pad, padlen + 8);
	for (i = 0; i < 5; i++) {
		digest[i * 4] = ctx->state[i] >> 24;
		digest[i * 4 + 1] = ctx->state[i] >> 16;
		digest[i * 4 + 2] = ctx->state[i] >> 8;
		digest[i * 4 + 3] = ctx->state[i];
	}
}

static void hmac_sha1(const uint8_t *key, size_t keylen, const void *data,
		      size_t length, uint8_t digest[20])
{
	uint8_t ipad[64], opad[64], khash[20];
	struct sha1_ctx ctx;
	unsigned int i;

	if (keylen > 64) {
		sha1_init(&ctx); sha1_update(&ctx, key, keylen);
		sha1_final(&ctx, khash); key = khash; keylen = sizeof(khash);
	}
	memset(ipad, 0x36, sizeof(ipad)); memset(opad, 0x5c, sizeof(opad));
	for (i = 0; i < keylen; i++) {
		ipad[i] ^= key[i]; opad[i] ^= key[i];
	}
	sha1_init(&ctx); sha1_update(&ctx, ipad, sizeof(ipad));
	sha1_update(&ctx, data, length); sha1_final(&ctx, digest);
	sha1_init(&ctx); sha1_update(&ctx, opad, sizeof(opad));
	sha1_update(&ctx, digest, 20); sha1_final(&ctx, digest);
}

static void derive_pmk(const char *pass, const char *ssid, uint8_t pmk[32])
{
	uint8_t input[36], u[20], block[20];
	size_t ssid_len = strlen(ssid), pass_len = strlen(pass);
	unsigned int index, round, i, offset = 0, copy;

	memcpy(input, ssid, ssid_len);
	for (index = 1; index <= 2; index++) {
		input[ssid_len] = index >> 24; input[ssid_len + 1] = index >> 16;
		input[ssid_len + 2] = index >> 8; input[ssid_len + 3] = index;
		hmac_sha1((const uint8_t *)pass, pass_len, input, ssid_len + 4, u);
		memcpy(block, u, sizeof(block));
		for (round = 1; round < 4096; round++) {
			hmac_sha1((const uint8_t *)pass, pass_len, u, sizeof(u), u);
			for (i = 0; i < sizeof(block); i++) block[i] ^= u[i];
		}
		copy = sizeof(block);
		if (copy > sizeof(uint8_t[32]) - offset) copy = 32 - offset;
		memcpy(pmk + offset, block, copy); offset += copy;
	}
	memset(input, 0, sizeof(input)); memset(u, 0, sizeof(u));
	memset(block, 0, sizeof(block));
}

static int add_attr(struct nlmsghdr *nlh, size_t capacity, uint16_t type,
		    const void *data, size_t length)
{
	size_t attr_len = NLA_HDRLEN + length, offset = NLMSG_ALIGN(nlh->nlmsg_len);
	struct nlattr *attr;

	if (offset + NLA_ALIGN_LEN(attr_len) > capacity) return -1;
	attr = (void *)((char *)nlh + offset); attr->nla_type = type;
	attr->nla_len = attr_len;
	if (length) memcpy(NLA_DATA_PTR(attr), data, length);
	memset((char *)attr + attr_len, 0, NLA_ALIGN_LEN(attr_len) - attr_len);
	nlh->nlmsg_len = offset + NLA_ALIGN_LEN(attr_len); return 0;
}

static struct nlattr *find_attr(void *data, int length, uint16_t type)
{
	struct nlattr *attr;

	for (attr = data; length >= (int)sizeof(*attr) &&
	     attr->nla_len >= sizeof(*attr) && attr->nla_len <= length;
	     length -= NLA_ALIGN_LEN(attr->nla_len),
	     attr = (void *)((char *)attr + NLA_ALIGN_LEN(attr->nla_len)))
		if ((attr->nla_type & NLA_TYPE_MASK) == type) return attr;
	return NULL;
}

static int send_request(int fd, struct nlmsghdr *nlh)
{
	struct sockaddr_nl peer = { .nl_family = AF_NETLINK };
	struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
	struct msghdr msg = { .msg_name = &peer, .msg_namelen = sizeof(peer),
		.msg_iov = &iov, .msg_iovlen = 1 };
	return sendmsg(fd, &msg, 0) < 0 ? -errno : 0;
}

static int receive_ack(int fd, uint32_t seq)
{
	char buffer[BUF_SIZE]; struct nlmsghdr *nlh; ssize_t length;
	for (;;) {
		length = recv(fd, buffer, sizeof(buffer), 0);
		if (length < 0) return -errno;
		for (nlh = (void *)buffer; NLMSG_OK(nlh, length);
		     nlh = NLMSG_NEXT(nlh, length))
			if (nlh->nlmsg_seq == seq && nlh->nlmsg_type == NLMSG_ERROR)
				return ((struct nlmsgerr *)NLMSG_DATA(nlh))->error;
	}
}

static int resolve_family(int fd)
{
	char buffer[BUF_SIZE] = { 0 }; struct nlmsghdr *nlh = (void *)buffer;
	struct genlmsghdr *genl; ssize_t length; uint32_t seq = ++sequence;
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN); nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST; nlh->nlmsg_seq = seq;
	genl = NLMSG_DATA(nlh); genl->cmd = CTRL_CMD_GETFAMILY; genl->version = 1;
	if (add_attr(nlh, sizeof(buffer), CTRL_ATTR_FAMILY_NAME,
		     NL80211_GENL_NAME, sizeof(NL80211_GENL_NAME)) || send_request(fd, nlh)) return -1;
	for (;;) {
		struct nlattr *family;
		length = recv(fd, buffer, sizeof(buffer), 0); if (length < 0) return -1;
		for (nlh = (void *)buffer; NLMSG_OK(nlh, length); nlh = NLMSG_NEXT(nlh, length)) {
			if (nlh->nlmsg_seq != seq || nlh->nlmsg_type == NLMSG_ERROR) continue;
			genl = NLMSG_DATA(nlh); family = find_attr((char *)genl + GENL_HDRLEN,
				NLMSG_PAYLOAD(nlh, GENL_HDRLEN), CTRL_ATTR_FAMILY_ID);
			if (family && NLA_PAYLOAD_LEN(family) >= 2) return *(uint16_t *)NLA_DATA_PTR(family);
		}
	}
}

static int connect_network(int fd, int family, uint32_t ifindex,
			   const char *ssid, const char *passphrase,
			   const uint8_t pmk[32],
			   const uint8_t *bssid, uint32_t frequency)
{
	char buffer[512] = { 0 }; struct nlmsghdr *nlh = (void *)buffer;
	struct genlmsghdr *genl; uint32_t seq = ++sequence;
	uint32_t wpa = WPA2_VERSION, cipher = CCMP_SUITE;
	uint32_t akm[] = { PSK_SUITE, SAE_SUITE };

	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN); nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK; nlh->nlmsg_seq = seq;
	genl = NLMSG_DATA(nlh); genl->cmd = NL80211_CMD_CONNECT; genl->version = 1;
	if (add_attr(nlh, sizeof(buffer), NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex)) ||
	    add_attr(nlh, sizeof(buffer), NL80211_ATTR_SSID, ssid, strlen(ssid)) ||
	    (passphrase[0] && (add_attr(nlh, sizeof(buffer), NL80211_ATTR_PRIVACY, NULL, 0) ||
	    add_attr(nlh, sizeof(buffer), NL80211_ATTR_WPA_VERSIONS, &wpa, sizeof(wpa)) ||
	    add_attr(nlh, sizeof(buffer), NL80211_ATTR_CIPHER_SUITES_PAIRWISE, &cipher, sizeof(cipher)) ||
	    add_attr(nlh, sizeof(buffer), NL80211_ATTR_CIPHER_SUITE_GROUP, &cipher, sizeof(cipher)) ||
	    add_attr(nlh, sizeof(buffer), NL80211_ATTR_AKM_SUITES, akm, sizeof(akm)) ||
	    add_attr(nlh, sizeof(buffer), NL80211_ATTR_PMK, pmk, 32) ||
	    add_attr(nlh, sizeof(buffer), NL80211_ATTR_SAE_PASSWORD,
		     passphrase, strlen(passphrase)))) ||
	    (bssid && add_attr(nlh, sizeof(buffer), NL80211_ATTR_MAC, bssid, 6)) ||
	    (frequency && add_attr(nlh, sizeof(buffer), NL80211_ATTR_WIPHY_FREQ,
				   &frequency, sizeof(frequency))) || send_request(fd, nlh)) return -1;
	return receive_ack(fd, seq);
}

int main(int argc, char **argv)
{
	struct sockaddr_nl local = { .nl_family = AF_NETLINK }; uint8_t pmk[32];
	struct ether_addr *address = NULL; uint32_t frequency = 0;
	unsigned int ifindex; int fd, family, ret;

	if (argc != 4 && argc != 6) {
		fprintf(stderr, "usage: %s interface ssid passphrase [bssid frequency]\n", argv[0]); return 2;
	}
	if (strlen(argv[2]) < 1 || strlen(argv[2]) > 32 ||
	    (strlen(argv[3]) && strlen(argv[3]) < 8) || strlen(argv[3]) > 63) {
		fprintf(stderr, "invalid SSID or WPA2 passphrase length\n"); return 2;
	}
	if (argc == 6) {
		address = ether_aton(argv[4]);
		frequency = strtoul(argv[5], NULL, 10);
		if (!address || frequency < 2412 || frequency > 2484) {
			fprintf(stderr, "invalid BSSID or 2.4 GHz frequency\n"); return 2;
		}
	}
	ifindex = if_nametoindex(argv[1]); if (!ifindex) { perror(argv[1]); return 1; }
	if (argv[3][0])
		derive_pmk(argv[3], argv[2], pmk);
	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0 || bind(fd, (void *)&local, sizeof(local))) { perror("netlink"); return 1; }
	family = resolve_family(fd); if (family < 0) { fprintf(stderr, "cannot resolve nl80211\n"); return 1; }
	ret = connect_network(fd, family, ifindex, argv[2], argv[3], pmk,
			      address ? address->ether_addr_octet : NULL, frequency);
	memset(pmk, 0, sizeof(pmk)); memset(argv[3], 0, strlen(argv[3]));
	if (ret) { errno = -ret; perror("NL80211_CMD_CONNECT"); return 1; }
	printf("association requested on %s\n", argv[1]); return 0;
}
