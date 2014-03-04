#include <vector>
#include <algorithm>
#include <stdexcept>
#include <string.h>
#include <errno.h>

#include <osv/types.h>
#include <osv/device.h>
#include <osv/bio.h>

#include "drivers/scsi-common.hh"

scsi_common_req::scsi_common_req(struct bio* bio, u16 _target, u16 _lun, u8 cmd)
    : bio(bio),
      target(_target),
      lun(_lun)
{
    bio->bio_private = this;
    bio->bio_cmd = BIO_SCSI;
    switch (cmd) {
        case CDB_CMD_READ_16:
        case CDB_CMD_WRITE_16: {
           u64 lba = bio->bio_offset / SCSI_SECTOR_SIZE;
           u32 count = bio->bio_bcount / SCSI_SECTOR_SIZE;
           struct cdb_readwrite_16 c;
           memset(&c, 0, sizeof(c));
           c.command = cmd;
           c.lba = htobe64(lba);
           c.count = htobe32(count);
           memcpy(cdb, &c, sizeof(c));
           break;
        }
        case CDB_CMD_SYNCHRONIZE_CACHE_10: {
           struct cdb_readwrite_10 c;
           memset(&c, 0, sizeof(c));
           c.command = cmd;
           c.lba = 0;
           c.count = 0;
           memcpy(cdb, &c, sizeof(c));
           break;
        }
        case CDB_CMD_INQUIRY: {
            struct cdb_inquery c;
            memset(&c, 0, sizeof(c));
            c.command = CDB_CMD_INQUIRY;
            c.alloc_len = htobe16(bio->bio_bcount);
            memcpy(cdb, &c, sizeof(c));
            break;
        }
        case CDB_CMD_READ_CAPACITY: {
            struct cdb_read_capacity c;
            memset(&c, 0, sizeof(c));
            c.command = CDB_CMD_READ_CAPACITY;
            c.service_action = 0x10;
            c.alloc_len = htobe32(bio->bio_bcount);
            memcpy(cdb, &c, sizeof(c));
            break;
        }
        case CDB_CMD_TEST_UNIT_READY: {
            struct cdb_test_unit_ready c;
            memset(&c, 0, sizeof(c));
            c.command = CDB_CMD_TEST_UNIT_READY;
            memcpy(cdb, &c, sizeof(c));
            break;
        }
        case CDB_CMD_REQUEST_SENSE: {
            struct cdb_request_sense c;
            memset(&c, 0, sizeof(c));
            c.command = CDB_CMD_REQUEST_SENSE;
            c.alloc_len = bio->bio_bcount;
            memcpy(cdb, &c, sizeof(c));
            break;
        }
        case CDB_CMD_REPORT_LUNS: {
            struct cdb_report_luns c;
            memset(&c, 0, sizeof(c));
            c.command = CDB_CMD_REPORT_LUNS;
            c.select_report = 0;
            c.alloc_len=htobe32(bio->bio_bcount);
            memcpy(cdb, &c, sizeof(c));
            break;
        }
        default:
            break;
    };
}

int scsi_common::exec_readwrite(u16 target, u16 lun, struct bio *bio, u8 cmd)
{
    alloc_scsi_req(bio, target, lun, cmd);

    return exec_cmd(bio);
}

int scsi_common::exec_synccache(u16 target, u16 lun, struct bio *bio, u8 cmd)
{
    alloc_scsi_req(bio, target, lun, cmd);

    return exec_cmd(bio);
}

void scsi_common::exec_inquery(u16 target, u16 lun)
{
    struct bio *bio = alloc_bio();
    if (!bio)
        throw std::runtime_error("Fail to alloate bio");

    auto data = new cdbres_inquiry;
    bio->bio_bcount = sizeof(*data);
    bio->bio_data = data;

    auto req = alloc_scsi_req(bio, target, lun, CDB_CMD_INQUIRY);

    make_request(bio);
    bio_wait(bio);
    destroy_bio(bio);

    auto response = req->response;
    if (response != SCSI_OK)
        throw std::runtime_error("Fail to exec_inquery");
}

void scsi_common::exec_read_capacity(u16 target, u16 lun, size_t &devsize)
{
    struct bio *bio = alloc_bio();
    if (!bio)
        throw std::runtime_error("Fail to allocate bio");

    auto data = new cdbres_read_capacity;
    bio->bio_bcount = sizeof(*data);
    bio->bio_data = data;

    auto req = alloc_scsi_req(bio, target, lun, CDB_CMD_READ_CAPACITY);

    make_request(bio);
    bio_wait(bio);
    destroy_bio(bio);

    // sectors returned by this cmd is the address of laster sector
    u64 sectors = be64toh(data->sectors) + 1;
    u32 blksize = be32toh(data->blksize);
    devsize = sectors * blksize;

    auto response = req->response;
    if (response != SCSI_OK)
        throw std::runtime_error("Fail to exec_read_capacity");

    delete req;
    delete data;
}

void scsi_common::exec_test_unit_ready(u16 target, u16 lun)
{
    struct bio *bio = alloc_bio();
    if (!bio)
        throw std::runtime_error("Fail to allocate bio");

    auto req = alloc_scsi_req(bio, target, lun, CDB_CMD_TEST_UNIT_READY);

    make_request(bio);
    bio_wait(bio);
    destroy_bio(bio);

    auto response = req->response;
    if (response != SCSI_OK)
        throw std::runtime_error("Fail to exec_test_unit_ready");

    auto status = req->status;
    if (status != SCSI_OK) {
        throw std::runtime_error("Fail to exec_test_unit_ready");
    }

    delete req;
}

void scsi_common::exec_request_sense(u16 target, u16 lun)
{

    struct bio *bio = alloc_bio();
    if (!bio)
        throw std::runtime_error("Fail to allocate bio");

    auto data = new cdbres_request_sense;
    bio->bio_bcount = sizeof(*data);
    bio->bio_data = data;

    auto req = alloc_scsi_req(bio, target, lun, CDB_CMD_REQUEST_SENSE);

    make_request(bio);
    bio_wait(bio);
    destroy_bio(bio);

    if (data->asc == 0x3a)
        printf("virtio-scsi: target %d lun %d reports medium not present\n", target, lun);

    auto response = req->response;
    if (response != SCSI_OK)
        throw std::runtime_error("Fail to exec_request_sense");

    delete req;
    delete data;
}

std::vector<u16> scsi_common::exec_report_luns(u16 target)
{
    std::vector<u16> luns;
    struct bio *bio = alloc_bio();
    if (!bio)
        throw std::runtime_error("Fail to allocate bio");

    auto data = new cdbres_report_luns;
    bio->bio_bcount = sizeof(*data);
    bio->bio_data = data;

    auto req = alloc_scsi_req(bio, target, 0, CDB_CMD_REPORT_LUNS);

    make_request(bio);
    bio_wait(bio);
    destroy_bio(bio);

    auto response = req->response;
    if (response != SCSI_OK) {
        throw std::runtime_error("Fail to exec_report_luns");
    }

    auto status = req->status;
    if (status == SCSI_OK) {
        auto list_len = be32toh(data->list_len);
        for (unsigned i = 0; i < list_len / 8; i++) {
            luns.push_back((data->list[i] & 0xffff) >> 8);
        }
    } else {
        // Report LUNS is not implemented
        for (u16 lun = 0; lun < config.max_lun; lun++) {
                luns.push_back(lun);
        }
    }
    std::sort(luns.begin(),luns.end());

    delete req;
    delete data;

    return luns;
}

bool scsi_common::cdb_data_in(const u8 *cdb)
{
   return cdb[0] != CDB_CMD_WRITE_16;
}

bool scsi_common::cdb_data_rw(const u8 *cdb)
{
   auto cmd = cdb[0];

   return cmd == CDB_CMD_WRITE_16 ||
          cmd == CDB_CMD_READ_16;
}

bool scsi_common::test_lun(u16 target, u16 lun)
{
    bool ready = false;
    u8 nr = 0;

    do {
        try {
            exec_inquery(target, lun);
            exec_test_unit_ready(target, lun);
        } catch (std::runtime_error err) {
            nr++;
            continue;
        }
        ready = true;
    } while (nr < 2 && !ready);

    return ready;
}

void scsi_common::scan()
{
    /* TODO: Support more target */
    for (u16 target = 1; target < 2; target++) {
        try {
            auto luns = exec_report_luns(target);
            for (auto &lun : luns) {
                add_lun(target, lun);
            }
        } catch(std::runtime_error err) {
            printf("scsi: %s\n", err.what());
        }
    }
}
