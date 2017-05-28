/* Copyright (c) 2007-2017 Dovecot authors, see the included COPYING file */
/* Copyright (c) 2017 Tallence AG and the authors, see the included COPYING file */

#include <string>
#include <map>
#include <vector>

#include <rados/librados.hpp>

extern "C" {
#include <stdio.h>
#include <utime.h>

#include "lib.h"
#include "typeof-def.h"

#include "array.h"
#include "hostpid.h"
#include "istream.h"
#include "istream-crlf.h"
#include "ostream.h"
#include "str.h"
#include "index-mail.h"
#include "rados-storage-local.h"
#include "rados-sync.h"
#include "debug-helper.h"
}

#include "rados-storage-struct.h"
#include "rados-storage.h"

using namespace librados;  // NOLINT

using std::string;

class rados_save_context {
 public:
  explicit rados_save_context(RadosStorage &rados_storage) : rados_storage(rados_storage) {}

  struct mail_save_context ctx;

  struct rados_mailbox *mbox;
  struct mail_index_transaction *trans;

  char *tmp_basename;
  unsigned int mail_count;
  guid_128_t mail_guid;

  struct rados_sync_context *sync_ctx;

  /* updated for each appended mail: */
  uint32_t seq;
  struct istream *input;

  RadosStorage &rados_storage;
  ObjectWriteOperation write_op;
  string cur_oid;
  std::vector<std::string> oids;

  unsigned int failed : 1;
  unsigned int finished : 1;
};

static const char *rados_get_save_path(struct rados_save_context *ctx, unsigned int num) {
  FUNC_START();
  const char *dir;
  guid_128_t guid_128;

  guid_128_generate(guid_128);
  const char *guid = guid_128_to_string(guid_128);

  dir = mailbox_get_path(&ctx->mbox->box);
  const char *ret = t_strdup_printf("%s/%s", dir, guid);
  i_debug("save path = %s", ret);
  debug_print_mail_save_context(&ctx->ctx, "rados-save::rados_get_save_path", NULL);
  FUNC_END();
  return ret;
}

struct mail_save_context *rados_save_alloc(struct mailbox_transaction_context *t) {
  FUNC_START();
  struct rados_mailbox *mbox = (struct rados_mailbox *)t->box;
  struct rados_storage *storage = (struct rados_storage *)mbox->storage;
  rados_save_context *ctx;

  i_assert((t->flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

  if (t->save_ctx == NULL) {
    ctx = new rados_save_context(*(storage->s));
    ctx->ctx.transaction = t;
    ctx->mbox = mbox;
    ctx->trans = t->itrans;
    guid_128_generate(ctx->mail_guid);
    t->save_ctx = &ctx->ctx;
  }
  debug_print_mail_save_context(t->save_ctx, "rados-save::rados_save_alloc", NULL);
  FUNC_END();
  return t->save_ctx;
}

int rados_save_begin(struct mail_save_context *_ctx, struct istream *input) {
  FUNC_START();
  rados_save_context *ctx = (struct rados_save_context *)_ctx;
  struct mailbox_transaction_context *trans = _ctx->transaction;
  enum mail_flags save_flags;
  struct istream *crlf_input;

  ctx->failed = FALSE;

  ctx->cur_oid = rados_get_save_path(ctx, ctx->mail_count);
  i_debug("rados_save_begin: saving to %s", ctx->cur_oid.c_str());

  // create RADOS object and set state to saving
  librados::bufferlist state_bl;
  state_bl.append("S");
  int ret = ctx->rados_storage.get_io_ctx().setxattr(ctx->cur_oid, "STATE", state_bl);
  if (ret < 0) {
    mail_storage_set_critical(trans->box->storage, "setxattr(%s, &s) failed: %m", ctx->cur_oid.c_str(), "STATE");
    ctx->failed = TRUE;
  }

  if (ctx->failed) {
    debug_print_mail_save_context(_ctx, "rados-save::rados_save_begin (ret -1, 1)", NULL);
    FUNC_END_RET("ret == -1");
    return -1;
  }

  /* add to index */
  save_flags = _ctx->data.flags & ~MAIL_RECENT;
  mail_index_append(ctx->trans, 0, &ctx->seq);
  mail_index_update_flags(ctx->trans, ctx->seq, MODIFY_REPLACE, save_flags);
  if (_ctx->data.keywords != NULL) {
    mail_index_update_keywords(ctx->trans, ctx->seq, MODIFY_REPLACE, _ctx->data.keywords);
  }
  if (_ctx->data.min_modseq != 0) {
    mail_index_update_modseq(ctx->trans, ctx->seq, _ctx->data.min_modseq);
  }

  mail_set_seq_saving(_ctx->dest_mail, ctx->seq);

  crlf_input = i_stream_create_crlf(input);
  ctx->input = index_mail_cache_parse_init(_ctx->dest_mail, crlf_input);
  i_stream_unref(&crlf_input);

  ctx->oids.push_back(ctx->cur_oid);

  debug_print_mail_save_context(_ctx, "rados-save::rados_save_begin", NULL);
  FUNC_END();
  return ctx->failed ? -1 : 0;
}

size_t rados_stream_mail_to_rados(rados_save_context *ctx) {
  const unsigned char *data;
  ssize_t ret;
  librados::bufferlist bl;
  size_t size;
  size_t total_size = 0;

  do {
    (void)i_stream_read_data(ctx->input, &data, &size, 0);
    i_debug("rados_stream_mail_to_rados: size=%d", size);
    if (size == 0) {
      /*all sent */
      if (ctx->input->stream_errno != 0) {
        return -1;
      }
      break;
    }

    bl.clear();
    bl.append((const char *)data, size);

    int err = ctx->rados_storage.get_io_ctx().append(ctx->cur_oid, bl, size);
    if (err < 0) {
      return -1;
    }
    total_size += size;
    i_stream_skip(ctx->input, 0);  // ???
  } while ((size_t)ret == size);

  return total_size;
}

int rados_save_continue(struct mail_save_context *_ctx) {
  FUNC_START();
  struct rados_save_context *ctx = (struct rados_save_context *)_ctx;
  struct mail_storage *storage = &ctx->mbox->storage->storage;

  if (ctx->failed) {
    debug_print_mail_save_context(_ctx, "rados-save::rados_save_continue (ret -1, 1)", NULL);
    debug_print_mail_storage(storage, "rados-save::rados_save_continue (ret -1, 1)", NULL);
    FUNC_END_RET("ret == -1");
    return -1;
  }

  do {
    if (rados_stream_mail_to_rados(ctx) < 0) {
      if (!mail_storage_set_error_from_errno(storage)) {
        mail_storage_set_critical(storage, "write(%s) failed: %m", rados_get_save_path(ctx, ctx->mail_count));
      }
      ctx->failed = TRUE;
      debug_print_mail_save_context(_ctx, "rados-save::rados_save_continue (ret -1, 2)", NULL);
      debug_print_mail_storage(storage, "rados-save::rados_save_continue (ret -1, 2)", NULL);
      FUNC_END_RET("ret == -1");
      return -1;
    }
    index_mail_cache_parse_continue(_ctx->dest_mail);

    /* both tee input readers may consume data from our primary
     input stream. we'll have to make sure we don't return with
     one of the streams still having data in them. */
  } while (i_stream_read(ctx->input) > 0);

  debug_print_mail_save_context(_ctx, "rados-save::rados_save_continue", NULL);
  debug_print_mail_storage(storage, "rados-save::rados_save_continue", NULL);
  FUNC_END();
  return 0;
}

static int rados_save_flush(struct rados_save_context *ctx, const char *path) {
  FUNC_START();
  struct mail_storage *storage = &ctx->mbox->storage->storage;
  struct stat st;
  int ret = 0;

  /*
 if (o_stream_nfinish(ctx->ctx.data.output) < 0) {
    mail_storage_set_critical(storage, "write(%s) failed: %s", path, o_stream_get_error(ctx->ctx.data.output));
    ret = -1;
  }


  if (storage->set->parsed_fsync_mode != FSYNC_MODE_NEVER) {
    if (fsync(ctx->fd) < 0) {
      mail_storage_set_critical(storage, "fsync(%s) failed: %m", path);
      ret = -1;
    }
  }

  if (ctx->ctx.data.received_date == (time_t)-1) {
    if (fstat(ctx->fd, &st) == 0) {
      ctx->ctx.data.received_date = st.st_mtime;
    } else {
      mail_storage_set_critical(storage, "fstat(%s) failed: %m", path);
      ret = -1;
    }
  } else {
    struct utimbuf ut;

    ut.actime = ioloop_time;
    ut.modtime = ctx->ctx.data.received_date;
    if (utime(path, &ut) < 0) {
      mail_storage_set_critical(storage, "utime(%s) failed: %m", path);
      ret = -1;
    }
  }

  o_stream_destroy(&ctx->ctx.data.output);
  */

  debug_print_mail_save_context(&ctx->ctx, "rados-save::rados_save_flush", NULL);
  debug_print_mail_storage(storage, "rados-save::rados_save_flush", NULL);
  FUNC_END();
  return ret;
}

int rados_save_finish(struct mail_save_context *_ctx) {
  FUNC_START();
  struct rados_save_context *ctx = (struct rados_save_context *)_ctx;
  struct mail_storage *storage = ((struct mail_save_context *)ctx)->transaction->box->storage;
  struct rbox_storage *rbox_ctx = (struct rbox_storage *)storage;

  ctx->finished = TRUE;

  if (rados_save_flush(ctx, ctx->cur_oid.c_str()) < 0)
    ctx->failed = TRUE;

  if (!ctx->failed) {
    ctx->mail_count++;
  } else {
    ctx->rados_storage.get_io_ctx().remove(ctx->cur_oid);
    // TODO(peter) cleanup all oids
  }

  index_mail_cache_parse_deinit(_ctx->dest_mail, _ctx->data.received_date, !ctx->failed);
  if (ctx->input != NULL)
    i_stream_unref(&ctx->input);

  index_save_context_free(_ctx);
  debug_print_mail_save_context(_ctx, "rados-save::rados_save_finish", NULL);
  FUNC_END();
  return ctx->failed ? -1 : 0;
}

void rados_save_cancel(struct mail_save_context *_ctx) {
  FUNC_START();
  struct rados_save_context *ctx = (struct rados_save_context *)_ctx;

  ctx->failed = TRUE;
  (void)rados_save_finish(_ctx);

  debug_print_mail_save_context(_ctx, "rados-save::rados_save_cancel", NULL);
  FUNC_END();
}

int rados_transaction_save_commit_pre(struct mail_save_context *_ctx) {
  FUNC_START();
  struct rados_save_context *ctx = (struct rados_save_context *)_ctx;
  struct mailbox_transaction_context *_t = _ctx->transaction;
  const struct mail_index_header *hdr;
  struct seq_range_iter iter;
  uint32_t uid;
  const char *dir;
  string_t *src_path, *dest_path;
  unsigned int n;
  size_t src_prefixlen, dest_prefixlen;

  i_assert(ctx->finished);

  if (rados_sync_begin(ctx->mbox, &ctx->sync_ctx, TRUE) < 0) {
    ctx->failed = TRUE;
    rados_transaction_save_rollback(_ctx);
    debug_print_mail_save_context(_ctx, "rados-save::rados_transaction_save_commit_pre (ret -1, 1)", NULL);
    FUNC_END_RET("ret == -1");
    return -1;
  }

  hdr = mail_index_get_header(ctx->sync_ctx->sync_view);
  mail_index_append_finish_uids(ctx->trans, hdr->next_uid, &_t->changes->saved_uids);
  _t->changes->uid_validity = ctx->sync_ctx->uid_validity;

  dir = mailbox_get_path(&ctx->mbox->box);

  src_path = t_str_new(256);
  str_printfa(src_path, "%s/%s.", dir, ctx->tmp_basename);
  src_prefixlen = str_len(src_path);

  dest_path = t_str_new(256);
  str_append(dest_path, dir);
  str_append_c(dest_path, '/');
  dest_prefixlen = str_len(dest_path);

  seq_range_array_iter_init(&iter, &_t->changes->saved_uids);
  n = 0;
  while (seq_range_array_iter_nth(&iter, n++, &uid) > 0) {
    str_truncate(src_path, src_prefixlen);
    str_truncate(dest_path, dest_prefixlen);
    str_printfa(src_path, "%u", n - 1);
    str_printfa(dest_path, "%u.", uid);

    i_debug("rename mail from %s", str_c(src_path));
    i_debug("              to %s", str_c(dest_path));

    if (rename(str_c(src_path), str_c(dest_path)) < 0) {
      mail_storage_set_critical(&ctx->mbox->storage->storage, "rename(%s, %s) failed: %m", str_c(src_path),
                                str_c(dest_path));
      ctx->failed = TRUE;
      rados_transaction_save_rollback(_ctx);
      debug_print_mail_save_context(_ctx, "rados-save::rados_transaction_save_commit_pre (ret -1, 2)", NULL);
      FUNC_END_RET("ret == -1");
      return -1;
    }
  }
  debug_print_mail_save_context(_ctx, "rados-save::rados_transaction_save_commit_pre", NULL);
  FUNC_END();
  return 0;
}

void rados_transaction_save_commit_post(struct mail_save_context *_ctx,
                                        struct mail_index_transaction_commit_result *result) {
  FUNC_START();
  struct rados_save_context *ctx = (struct rados_save_context *)_ctx;

  _ctx->transaction = NULL; /* transaction is already freed */

  mail_index_sync_set_commit_result(ctx->sync_ctx->index_sync_ctx, result);

  (void)rados_sync_finish(&ctx->sync_ctx, TRUE);
  debug_print_mail_save_context(_ctx, "rados-save::rados_transaction_save_commit_post", NULL);
  rados_transaction_save_rollback(_ctx);
  FUNC_END();
}

void rados_transaction_save_rollback(struct mail_save_context *_ctx) {
  FUNC_START();
  struct rados_save_context *ctx = (struct rados_save_context *)_ctx;

  if (!ctx->finished)
    rados_save_cancel(&ctx->ctx);

  if (ctx->sync_ctx != NULL)
    (void)rados_sync_finish(&ctx->sync_ctx, FALSE);

  debug_print_mail_save_context(_ctx, "rados-save::rados_transaction_save_rollback", NULL);

  i_free(ctx->tmp_basename);
  delete ctx;
  FUNC_END();
}