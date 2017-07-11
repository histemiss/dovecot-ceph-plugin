/* Copyright (c) 2017 Tallence AG and the authors, see the included COPYING file */

#ifndef SRC_STORAGE_RBOX_RBOX_SAVE_H_
#define SRC_STORAGE_RBOX_RBOX_SAVE_H_

#include <string>

#include "mail-storage-private.h"

#include "rados-storage.h"
#include "rados-mail-object.h"

class rbox_save_context {
 public:
  explicit rbox_save_context(librmb::RadosStorage &rados_storage)
      : ctx({}),
        mbox(NULL),
        trans(NULL),
        mail_count(0),
        sync_ctx(NULL),
        seq(0),
        input(NULL),
        rados_storage(rados_storage),
        current_object(NULL),
        failed(1),
        finished(1),
        copying(0),
		mail_buffer(NULL) {}

  struct mail_save_context ctx;

  struct rbox_mailbox *mbox;
  struct mail_index_transaction *trans;

  unsigned int mail_count;

  guid_128_t mail_guid;  // goes to index record
  guid_128_t mail_oid;   // goes to index record

  struct rbox_sync_context *sync_ctx;

  /* updated for each appended mail: */
   uint32_t seq;
   struct istream *input;

   librmb::RadosStorage &rados_storage;
   std::vector<librmb::RadosMailObject *> objects;
   librmb::RadosMailObject *current_object;
   buffer_t *mail_buffer;

   unsigned int failed : 1;
   unsigned int finished : 1;
   unsigned int copying : 1;
};

void rbox_add_to_index(struct mail_save_context *_ctx);

#endif /* SRC_STORAGE_RBOX_RBOX_SAVE_H_ */
