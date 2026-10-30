/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FreeBSD 9's sys/cdio.h for Wine's Unix side on the PS4 (BSD-2-Clause
 * facts, restated): dlls/ntdll/unix/cdrom.c compiles its FreeBSD branch
 * against these structures and ioctl codes under __FreeBSD__. The console
 * has no CD-ROM device Wine could open, so none of these ioctls is ever
 * issued; the header exists so that the file compiles. Encoded with
 * orbis-compat's sys/ioccom.h (FreeBSD's _IO* layout). */
#ifndef _ORBIS_WINE_SYS_CDIO_H
#define _ORBIS_WINE_SYS_CDIO_H

#include <sys/ioccom.h>

union msf_lba {
    struct {
        unsigned char unused;
        unsigned char minute;
        unsigned char second;
        unsigned char frame;
    } msf;
    int lba;
    unsigned char addr[4];
};

struct cd_toc_entry {
    unsigned char :8;
    unsigned char control:4;
    unsigned char addr_type:4;
    unsigned char track;
    unsigned char :8;
    union msf_lba addr;
};

struct cd_sub_channel_header {
    unsigned char :8;
    unsigned char audio_status;
#define CD_AS_AUDIO_INVALID    0x00
#define CD_AS_PLAY_IN_PROGRESS 0x11
#define CD_AS_PLAY_PAUSED      0x12
#define CD_AS_PLAY_COMPLETED   0x13
#define CD_AS_PLAY_ERROR       0x14
#define CD_AS_NO_STATUS        0x15
    unsigned char data_len[2];
};

struct cd_sub_channel_position_data {
    unsigned char data_format;
    unsigned char control:4;
    unsigned char addr_type:4;
    unsigned char track_number;
    unsigned char index_number;
    union msf_lba absaddr;
    union msf_lba reladdr;
};

struct cd_sub_channel_media_catalog {
    unsigned char data_format;
    unsigned char :8;
    unsigned char :8;
    unsigned char :8;
    unsigned char :7;
    unsigned char mc_valid:1;
    unsigned char mc_number[15];
};

struct cd_sub_channel_track_info {
    unsigned char data_format;
    unsigned char :8;
    unsigned char track_number;
    unsigned char :8;
    unsigned char :7;
    unsigned char ti_valid:1;
    unsigned char ti_number[15];
};

struct cd_sub_channel_info {
    struct cd_sub_channel_header header;
    union {
        struct cd_sub_channel_position_data position;
        struct cd_sub_channel_media_catalog media_catalog;
        struct cd_sub_channel_track_info track_info;
    } what;
};

struct ioc_toc_header {
    unsigned short len;
    unsigned char starting_track;
    unsigned char ending_track;
};

struct ioc_read_toc_entry {
    unsigned char address_format;
#define CD_LBA_FORMAT 1
#define CD_MSF_FORMAT 2
    unsigned char starting_track;
    unsigned short data_len;
    struct cd_toc_entry *data;
};

struct ioc_read_toc_single_entry {
    unsigned char address_format;
    unsigned char track;
    struct cd_toc_entry entry;
};

struct ioc_read_subchannel {
    unsigned char address_format;
    unsigned char data_format;
#define CD_SUBQ_DATA        0
#define CD_CURRENT_POSITION 1
#define CD_MEDIA_CATALOG    2
#define CD_TRACK_INFO       3
    unsigned char track;
    int data_len;
    struct cd_sub_channel_info *data;
};

struct ioc_play_track {
    unsigned char start_track;
    unsigned char start_index;
    unsigned char end_track;
    unsigned char end_index;
};

struct ioc_play_blocks {
    int blk;
    int len;
};

struct ioc_play_msf {
    unsigned char start_m;
    unsigned char start_s;
    unsigned char start_f;
    unsigned char end_m;
    unsigned char end_s;
    unsigned char end_f;
};

struct ioc_vol {
    unsigned char vol[4];
};

#define CDIOCPLAYTRACKS      _IOW('c', 1, struct ioc_play_track)
#define CDIOCPLAYBLOCKS      _IOW('c', 2, struct ioc_play_blocks)
#define CDIOCREADSUBCHANNEL  _IOWR('c', 3, struct ioc_read_subchannel)
#define CDIOREADTOCHEADER    _IOR('c', 4, struct ioc_toc_header)
#define CDIOREADTOCENTRYS    _IOWR('c', 5, struct ioc_read_toc_entry)
#define CDIOREADTOCENTRY     _IOWR('c', 6, struct ioc_read_toc_single_entry)
#define CDIOCSETPATCH        _IOW('c', 9, struct ioc_patch)
#define CDIOCGETVOL          _IOR('c', 10, struct ioc_vol)
#define CDIOCSETVOL          _IOW('c', 11, struct ioc_vol)
#define CDIOCSETMONO         _IO('c', 12)
#define CDIOCSETSTEREO       _IO('c', 13)
#define CDIOCSETMUTE         _IO('c', 14)
#define CDIOCSETLEFT         _IO('c', 15)
#define CDIOCSETRIGHT        _IO('c', 16)
#define CDIOCSETDEBUG        _IO('c', 17)
#define CDIOCCLRDEBUG        _IO('c', 18)
#define CDIOCPAUSE           _IO('c', 19)
#define CDIOCRESUME          _IO('c', 20)
#define CDIOCRESET           _IO('c', 21)
#define CDIOCSTART           _IO('c', 22)
#define CDIOCSTOP            _IO('c', 23)
#define CDIOCEJECT           _IO('c', 24)
#define CDIOCALLOW           _IO('c', 25)
#define CDIOCPREVENT         _IO('c', 26)
#define CDIOCCLOSE           _IO('c', 27)
#define CDIOCPLAYMSF         _IOW('c', 28, struct ioc_play_msf)

#endif /* _ORBIS_WINE_SYS_CDIO_H */
