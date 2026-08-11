// SPDX-License-Identifier: GPL-2.0-only
/* Minimal dependency-free nl80211 scan smoke test for ESP32-S31. */

#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 8192
#define NLA_ALIGNTO 4
#define NLA_ALIGN_LEN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_DATA_PTR(nla) ((void *)((char *)(nla) + NLA_HDRLEN))
#define NLA_PAYLOAD_LEN(nla) ((int)(nla)->nla_len - NLA_HDRLEN)

static uint32_t sequence;

static int add_attr(struct nlmsghdr *nlh, size_t capacity, uint16_t type,
		    const void *data, size_t length)
{
	size_t attr_len = NLA_HDRLEN + length;
	size_t offset = NLMSG_ALIGN(nlh->nlmsg_len);
	struct nlattr *attr;

	if (offset + NLA_ALIGN_LEN(attr_len) > capacity)
		return -1;
	attr = (struct nlattr *)((char *)nlh + offset);
	attr->nla_type = type;
	attr->nla_len = attr_len;
	if (length)
		memcpy(NLA_DATA_PTR(attr), data, length);
	memset((char *)attr + attr_len, 0, NLA_ALIGN_LEN(attr_len) - attr_len);
	nlh->nlmsg_len = offset + NLA_ALIGN_LEN(attr_len);
	return 0;
}

static struct nlattr *find_attr(void *data, int length, uint16_t type)
{
	struct nlattr *attr;

	for (attr = data; length >= (int)sizeof(*attr) &&
	     attr->nla_len >= sizeof(*attr) && attr->nla_len <= length;
	     length -= NLA_ALIGN_LEN(attr->nla_len),
	     attr = (void *)((char *)attr + NLA_ALIGN_LEN(attr->nla_len))) {
		if ((attr->nla_type & NLA_TYPE_MASK) == type)
			return attr;
	}
	return NULL;
}

static int send_request(int fd, struct nlmsghdr *nlh)
{
	struct sockaddr_nl peer = { .nl_family = AF_NETLINK };
	struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
	struct msghdr msg = {
		.msg_name = &peer, .msg_namelen = sizeof(peer),
		.msg_iov = &iov, .msg_iovlen = 1,
	};

	return sendmsg(fd, &msg, 0) < 0 ? -errno : 0;
}

static int recv_ack(int fd, uint32_t seq)
{
	char buffer[BUF_SIZE];
	ssize_t length;
	struct nlmsghdr *nlh;

	for (;;) {
		length = recv(fd, buffer, sizeof(buffer), 0);
		if (length < 0)
			return -errno;
		for (nlh = (void *)buffer; NLMSG_OK(nlh, length);
		     nlh = NLMSG_NEXT(nlh, length)) {
			struct nlmsgerr *error;

			if (nlh->nlmsg_seq != seq || nlh->nlmsg_type != NLMSG_ERROR)
				continue;
			error = NLMSG_DATA(nlh);
			return error->error;
		}
	}
}

static int resolve_family(int fd)
{
	char buffer[BUF_SIZE] = { 0 };
	struct nlmsghdr *nlh = (void *)buffer;
	struct genlmsghdr *genl;
	ssize_t length;
	uint32_t seq = ++sequence;

	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = seq;
	genl = NLMSG_DATA(nlh);
	genl->cmd = CTRL_CMD_GETFAMILY;
	genl->version = 1;
	if (add_attr(nlh, sizeof(buffer), CTRL_ATTR_FAMILY_NAME,
		     NL80211_GENL_NAME, sizeof(NL80211_GENL_NAME)) ||
	    send_request(fd, nlh))
		return -1;

	for (;;) {
		struct nlattr *family;

		length = recv(fd, buffer, sizeof(buffer), 0);
		if (length < 0)
			return -1;
		for (nlh = (void *)buffer; NLMSG_OK(nlh, length);
		     nlh = NLMSG_NEXT(nlh, length)) {
			if (nlh->nlmsg_seq != seq)
				continue;
			if (nlh->nlmsg_type == NLMSG_ERROR)
				return -1;
			genl = NLMSG_DATA(nlh);
			family = find_attr((char *)genl + GENL_HDRLEN,
				NLMSG_PAYLOAD(nlh, GENL_HDRLEN), CTRL_ATTR_FAMILY_ID);
			if (family && NLA_PAYLOAD_LEN(family) >= 2)
				return *(uint16_t *)NLA_DATA_PTR(family);
		}
	}
}

static int trigger_scan(int fd, int family, uint32_t ifindex)
{
	char buffer[256] = { 0 };
	struct nlmsghdr *nlh = (void *)buffer;
	struct genlmsghdr *genl;
	struct nlattr *ssids;
	uint32_t seq = ++sequence;
	size_t start;

	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = seq;
	genl = NLMSG_DATA(nlh);
	genl->cmd = NL80211_CMD_TRIGGER_SCAN;
	genl->version = 1;
	if (add_attr(nlh, sizeof(buffer), NL80211_ATTR_IFINDEX,
		     &ifindex, sizeof(ifindex)))
		return -1;
	start = NLMSG_ALIGN(nlh->nlmsg_len);
	if (add_attr(nlh, sizeof(buffer), NL80211_ATTR_SCAN_SSIDS | NLA_F_NESTED,
		     NULL, 0))
		return -1;
	ssids = (void *)(buffer + start);
	if (add_attr(nlh, sizeof(buffer), 1, NULL, 0))
		return -1;
	ssids->nla_len = (char *)nlh + nlh->nlmsg_len - (char *)ssids;
	if (send_request(fd, nlh))
		return -1;
	return recv_ack(fd, seq);
}

static void print_bss(struct genlmsghdr *genl, int payload)
{
	struct nlattr *bss = find_attr((char *)genl + GENL_HDRLEN, payload,
				       NL80211_ATTR_BSS);
	struct nlattr *bssid, *frequency, *signal, *ies;
	char ssid[33] = "<hidden>";
	uint8_t *ie, *end;

	if (!bss)
		return;
	bssid = find_attr(NLA_DATA_PTR(bss), NLA_PAYLOAD_LEN(bss),
			  NL80211_BSS_BSSID);
	frequency = find_attr(NLA_DATA_PTR(bss), NLA_PAYLOAD_LEN(bss),
			      NL80211_BSS_FREQUENCY);
	signal = find_attr(NLA_DATA_PTR(bss), NLA_PAYLOAD_LEN(bss),
			   NL80211_BSS_SIGNAL_MBM);
	ies = find_attr(NLA_DATA_PTR(bss), NLA_PAYLOAD_LEN(bss),
			NL80211_BSS_INFORMATION_ELEMENTS);
	if (!bssid || NLA_PAYLOAD_LEN(bssid) < 6)
		return;
	if (ies) {
		ie = NLA_DATA_PTR(ies);
		end = ie + NLA_PAYLOAD_LEN(ies);
		while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
			if (ie[0] == 0 && ie[1] <= 32) {
				memcpy(ssid, ie + 2, ie[1]);
				ssid[ie[1]] = '\0';
				break;
			}
			ie += 2 + ie[1];
		}
	}
	printf("%02x:%02x:%02x:%02x:%02x:%02x freq=%u signal=%.2f SSID=%s\n",
	       ((uint8_t *)NLA_DATA_PTR(bssid))[0],
	       ((uint8_t *)NLA_DATA_PTR(bssid))[1],
	       ((uint8_t *)NLA_DATA_PTR(bssid))[2],
	       ((uint8_t *)NLA_DATA_PTR(bssid))[3],
	       ((uint8_t *)NLA_DATA_PTR(bssid))[4],
	       ((uint8_t *)NLA_DATA_PTR(bssid))[5],
	       frequency ? *(uint32_t *)NLA_DATA_PTR(frequency) : 0,
	       signal ? *(int32_t *)NLA_DATA_PTR(signal) / 100.0 : 0.0, ssid);
}

static int dump_scan(int fd, int family, uint32_t ifindex)
{
	char buffer[BUF_SIZE] = { 0 };
	struct nlmsghdr *nlh = (void *)buffer;
	struct genlmsghdr *genl;
	uint32_t seq = ++sequence;
	int count = 0;

	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = seq;
	genl = NLMSG_DATA(nlh);
	genl->cmd = NL80211_CMD_GET_SCAN;
	genl->version = 1;
	if (add_attr(nlh, sizeof(buffer), NL80211_ATTR_IFINDEX,
		     &ifindex, sizeof(ifindex)) || send_request(fd, nlh))
		return -1;

	for (;;) {
		ssize_t length = recv(fd, buffer, sizeof(buffer), 0);

		if (length < 0)
			return -1;
		for (nlh = (void *)buffer; NLMSG_OK(nlh, length);
		     nlh = NLMSG_NEXT(nlh, length)) {
			if (nlh->nlmsg_seq != seq)
				continue;
			if (nlh->nlmsg_type == NLMSG_DONE) {
				printf("scan results: %d BSS\n", count);
				return count ? 0 : 2;
			}
			if (nlh->nlmsg_type == NLMSG_ERROR)
				return -1;
			genl = NLMSG_DATA(nlh);
			print_bss(genl, NLMSG_PAYLOAD(nlh, GENL_HDRLEN));
			count++;
		}
	}
}

int main(int argc, char **argv)
{
	const char *interface = argc > 1 ? argv[1] : "wlan0";
	struct sockaddr_nl local = { .nl_family = AF_NETLINK };
	unsigned int ifindex = if_nametoindex(interface);
	int fd, family, ret;

	if (!ifindex) {
		fprintf(stderr, "%s: interface not found\n", interface);
		return 1;
	}
	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0 || bind(fd, (void *)&local, sizeof(local))) {
		perror("netlink");
		return 1;
	}
	family = resolve_family(fd);
	if (family < 0) {
		fprintf(stderr, "cannot resolve nl80211\n");
		return 1;
	}
	ret = trigger_scan(fd, family, ifindex);
	if (ret) {
		errno = -ret;
		perror("NL80211_CMD_TRIGGER_SCAN");
		return 1;
	}
	printf("scan triggered on %s; waiting for completion\n", interface);
	sleep(6);
	return dump_scan(fd, family, ifindex);
}
