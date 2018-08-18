#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <sys/time.h>

#include <tox/tox.h>
#include <tox/toxav.h>
#include <sodium.h>

#include "utlist.h"

#define MAX_FILE_TRANSFER_SIZE 40000
#define MAX_FILE_TRANSFERS_PER_USER 4

#define MAX_CUSTOM_PACKETS_CACHED_BYTES_PER_USER 400000

// this is the maximum amount of transfers (CUSTOM PACKETS or FILES)
#define MAX_TOTAL_TRANSFERS 30

static uint64_t last_purge;
static uint64_t start_time;
static bool     signal_exit  = false;
static bool     is_connected = false;

static const int32_t audio_bitrate = 48;
static const int32_t video_bitrate = 5000;
static const char   *data_filename = "data";

static Tox   *g_tox   = NULL;
static ToxAV *g_toxAV = NULL;

// data packets
pthread_mutex_t data_packets_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct packet {
  uint8_t       *data;
  size_t         lenght;
  bool           lossless;
  struct packet *next, *prev;
} packet_t;


typedef struct file_transfer {
  uint32_t              file_number;
  uint32_t              kind;
  uint64_t              file_size;
  uint64_t              sent_pos;
  uint8_t              *filename;
  size_t                filename_length;
  uint8_t              *file_id;
  uint8_t              *data;
  bool                  recv_complete;
  int64_t               last_recv_time; // in ms
  bool                  send_started;
  bool                  send_complete;
  bool                  is_error;
  int64_t               last_send_time; // in ms
  struct file_transfer *next, *prev;
} file_transfer_t;

typedef struct friend {
  int64_t          last_activity; // in ms
  uint32_t         friend_number;
  uint64_t         data_packets_cached_bytes;
  packet_t        *data_packets;
  file_transfer_t *file_transfers;
  struct friend   *next, *prev; /* needed for singly- or doubly-linked lists */
} friend;

int friendscmp(friend *a, friend *b) {
  return a->friend_number == b->friend_number ? 0 : 1;
}

static friend *data_from_frnds = NULL;

static int64_t time_spec_to_ms(struct timespec *ts)
{
  return (int64_t)(ts->tv_sec * 1000) + (int64_t)(ts->tv_nsec / 1000000.0);
}

static int64_t monotonic_time_in_ms() {
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now)) exit(-1);
  return time_spec_to_ms(&now);
}

int64_t time_since(int64_t start_time_in_ms) {
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now)) exit(-1);
  int64_t res = time_spec_to_ms(&now) - start_time_in_ms;

  // one line?
  if (res < 0) {
    return 0;
  } else {
    return res;
  }
}

void friend_cleanup(Tox *tox)
{
  uint32_t friend_count = tox_self_get_friend_list_size(tox);

  if (friend_count == 0) {
    return;
  }

  uint32_t friends[friend_count];
  tox_self_get_friend_list(tox, friends);

  uint64_t curr_time = time(NULL);

  for (uint32_t i = 0; i < friend_count; i++) {
    TOX_ERR_FRIEND_GET_LAST_ONLINE err;
    uint32_t friend      = friends[i];
    uint64_t last_online = tox_friend_get_last_online(tox, friend, &err);

    if (err != TOX_ERR_FRIEND_GET_LAST_ONLINE_OK) {
      printf("couldn't obtain 'last online', this should never happen\n");
      continue;
    }

    if (curr_time - last_online > 2629743) {
      printf("removing friend %d\n", friend);
      tox_friend_delete(tox, friend, NULL);
    }
  }
}

void clean_file_transfer(file_transfer_t *ft) {
  if (ft == NULL) return;

  if (ft->file_id) free(ft->file_id);

  if (ft->filename) free(ft->filename);

  if (ft->data) free(ft->data);

  free(ft);
}

void purge_file_transfers(friend *f) {
  if (f != NULL) {
    file_transfer_t *el, *tmp;
    DL_FOREACH_SAFE(f->file_transfers, el, tmp) {
      if (el != NULL) {
        DL_DELETE(f->file_transfers, el);
        clean_file_transfer(el);
      }
    }
    f->file_transfers = NULL;
  }
}

friend* get_friend(uint32_t friend_number, bool create) {
  friend tmp, *elt = NULL;

  tmp.friend_number = friend_number;

  CDL_SEARCH(data_from_frnds, elt, &tmp, friendscmp);

  if (!create) return elt;


  if (elt == NULL) {
    // check if too many before creating
    int ii = 0;
    CDL_COUNT(data_from_frnds, elt, ii);

    if (ii > MAX_TOTAL_TRANSFERS) {
      return NULL;
    }


    elt                            = (friend *)calloc(1, sizeof(friend));
    elt->friend_number             = friend_number;
    elt->data_packets              = NULL;
    elt->file_transfers            = NULL;
    elt->last_activity             = INT64_MAX;
    elt->data_packets_cached_bytes = 0;
    CDL_PREPEND(data_from_frnds, elt);
  }

  return elt;
}

bool save_profile(Tox *tox)
{
  uint32_t save_size = tox_get_savedata_size(tox);
  uint8_t  save_data[save_size];

  tox_get_savedata(tox, save_data);

  FILE *file = fopen(data_filename, "wb");

  if (file) {
    fwrite(save_data, sizeof(uint8_t), save_size, file);
    fclose(file);
    return true;
  } else {
    printf("Could not write data to disk\n");
    return false;
  }
}

bool send_packets(Tox *tox, friend *f, TOX_ERR_FRIEND_CUSTOM_PACKET *error) {
  int count;

  if (!f) return false;

  packet_t *p;
  DL_COUNT(f->data_packets, p, count);

  if (count <= 0) return true;

  // get head
  p = f->data_packets;

  // this should not happen1
  if (p == NULL) return true;

  bool rv = false;

  if (p->lossless) {
    rv = tox_friend_send_lossless_packet(tox,
                                         f->friend_number,
                                         p->data,
                                         p->lenght,
                                         error);
  } else {
    rv = tox_friend_send_lossy_packet(tox, f->friend_number, p->data, p->lenght,
                                      error);

    // just one packet per iteration in lossy mode?
    // rv = false;
  }

  if (*error == TOX_ERR_FRIEND_CUSTOM_PACKET_OK)
  {
    f->data_packets_cached_bytes =
      ((int64_t)f->data_packets_cached_bytes - (int64_t)p->lenght > 0 ?
       f->data_packets_cached_bytes - p->lenght : 0);
    DL_DELETE(f->data_packets, p);

    if (p->data) free(p->data);

    if (p) free(p);
  }
  else
  {
    /* If this branch is ran, most likely we've hit congestion control. */
    if (*error == TOX_ERR_FRIEND_CUSTOM_PACKET_SENDQ)
    {
      printf("Failed to send packet to friend %d (Packet queue is full)\n", 0);
    }
    else if (*error == TOX_ERR_FRIEND_CUSTOM_PACKET_FRIEND_NOT_CONNECTED)
    {
      printf("Failed to send packet to friend %d (Friend gone)\n", 0);
    }
    else
    {
      printf("Failed to send packet to friend %d (err: %u)\n", 0, *error);

      if (*error == TOX_ERR_FRIEND_CUSTOM_PACKET_TOO_LONG) printf("Too long\n");
    }
  }
  return rv;
}

void file_chunk_request(Tox     *tox,
                        uint32_t friend_number,
                        uint32_t file_number,
                        uint64_t position,
                        size_t   length,
                        void    *user_data) {
  friend *f = get_friend(friend_number, false);

  if (f) {
    file_transfer_t *ft, *tmp;
    DL_FOREACH_SAFE(f->file_transfers, ft, tmp) {
      if (length == 0) {
        ft->send_complete = true;
        break;
      }

      if (ft->file_number == file_number) {
        TOX_ERR_FILE_SEND_CHUNK error;
        tox_file_send_chunk(tox,
                            friend_number,
                            file_number,
                            position,
                            ft->data + position,
                            length,
                            &error);

        if (error == TOX_ERR_FILE_SEND_CHUNK_OK) {
          ft->last_send_time = monotonic_time_in_ms();
        } else {
          ft->is_error = true;
        }

        // TODO: check error
        break;
      }

      // TODO, check odd case
    }
  }
}

void send_files(Tox *tox, friend *f, TOX_ERR_FILE_SEND *error) {
  file_transfer_t *ft, *tmp;

  printf("sendfile\n");

  if (f->file_transfers) {
    // do the cleanup
    DL_FOREACH_SAFE(f->file_transfers, ft, tmp) {
      //
      if ((time_since(ft->last_send_time) > 2001) ||
          (time_since(ft->last_recv_time) > 2001)) {
        // TODO: too long no send/recv, cleanup on stall?
      }

      if (ft->is_error) {
        const char *msg = "Problem with transfer, sorry...";
        tox_friend_send_message(tox, f->friend_number, TOX_MESSAGE_TYPE_NORMAL,
                                (uint8_t *)msg, strlen(msg), NULL);
        tox_file_control(tox,
                         f->friend_number,
                         ft->file_number,
                         TOX_FILE_CONTROL_CANCEL,
                         NULL);
        DL_DELETE(f->file_transfers, ft);
        clean_file_transfer(ft);
      }

      if (ft->recv_complete && !ft->send_started) {
        const char *msg = "Here's your file...";
        tox_friend_send_message(tox, f->friend_number, TOX_MESSAGE_TYPE_NORMAL,
                                (uint8_t *)msg, strlen(msg), NULL);

        ft->file_number = tox_file_send(tox,
                                        f->friend_number,
                                        TOX_FILE_KIND_DATA,
                                        ft->file_size,
                                        NULL,
                                        ft->filename,
                                        ft->filename_length,
                                        error);

        // check error or delete!!
        printf("Error in file send %d\n", *error);
        uint8_t *file_id = malloc(sizeof(uint8_t) * TOX_FILE_ID_LENGTH);
        TOX_ERR_FILE_GET error;
        tox_file_get_file_id(tox,
                             f->friend_number,
                             ft->file_number,
                             file_id,
                             &error);

        if (ft->file_id) free(ft->file_id);
        /* Update file id info */
        ft->file_id      = file_id;
        ft->send_started = true;
      }

      if (ft->send_complete) {
        DL_DELETE(f->file_transfers, ft);
        clean_file_transfer(ft);
      }
    }
  }
}

static void* run_toxav(void *arg)
{
  ToxAV *toxav = (ToxAV *)arg;

  for (;;) {
    toxav_iterate(toxav);

    long long time = toxav_iteration_interval(toxav) * 1000000L;
    nanosleep((const struct timespec[]) {{ 0, time } }, NULL);
  }

  return NULL;
}

void purge_packets(friend *elt) {
  packet_t *el, *tmp;

  if (elt->data_packets) {
    DL_FOREACH_SAFE(elt->data_packets, el, tmp) {
      DL_DELETE(elt->data_packets, el);

      if (el->data) free(el->data);

      if (el) free(el);
    }
    elt->data_packets = NULL;
  }
}

static void* run_tox(void *arg)
{
  Tox *tox = (Tox *)arg;
  TOX_ERR_FRIEND_CUSTOM_PACKET error;
  long long time;        // in ms
  long long us_diff = 0; // in us
  friend   *elt, *tmp1, *tmp2;
  bool ret = false;

  // TODO get rid of duplications in time
  struct timeval tv, tv_start;

  for (;;) {
    tox_iterate(tox, NULL);
    time = tox_iteration_interval(tox);

    // pthread_mutex_lock(&tox_iterate_mutex);


    gettimeofday(&tv_start, NULL);
    us_diff = 0;

    if (is_connected) {
      // Loop on transfers... TODO: first are favored
      CDL_FOREACH_SAFE(data_from_frnds, elt, tmp1, tmp2) {
        // do files stuff
        if (elt->file_transfers != NULL) {
          TOX_ERR_FILE_SEND error;
          send_files(tox, elt, &error);

          // TODO errors?
        }

        // cleanup inactive transfers first
        if ((elt->data_packets == NULL) && (elt->file_transfers == NULL)) {
          if (time_since(elt->last_activity) > 3000) {
            CDL_DELETE(data_from_frnds, elt);
            free(elt);
            continue;
          }
        } else {
          elt->last_activity = monotonic_time_in_ms();
        }
      }

      friend *it                = data_from_frnds;
      bool    still_got_packets = false;

      while (1) {
        if (it == NULL) {
          break;
        } else {
          elt = it->next;
        }


        if (elt->data_packets != NULL) {
          still_got_packets = true;
          ret               = send_packets(tox, elt, &error);

          if ((ret == false) &&
              (error == TOX_ERR_FRIEND_CUSTOM_PACKET_FRIEND_NOT_CONNECTED)) {
            purge_packets(elt);
            purge_file_transfers(elt);

            // remove friend from list of packets
            CDL_DELETE(data_from_frnds, elt);

            if (data_from_frnds != NULL) {
              it = elt->next;
            } else {
              it = NULL;
            }
            free(elt);
            break;
          }

          if (!ret) break;
        }

        if (elt == data_from_frnds) {
          if (still_got_packets == false) {
            // printf("No more packets, quit loop\n");
            break;
          }
          else still_got_packets = false;
        }

        gettimeofday(&tv, NULL);
        us_diff = 1000000 * (tv_start.tv_sec - tv.tv_sec) +
                  (tv_start.tv_usec - tv.tv_usec);

        if ((us_diff / 1000) > time) break;

        if (it->next) {
          it = it->next;
        } else if (it->prev) {
          it = it->prev;
          printf("got prev..unexpected...\n");
        } else {
          break;
        }
      }

      //// FILE TRANSFERS
      // int ii=0;
      // DL_COUNT(data_from_frnds,elt,ii);
      // printf("Friends count: %d\n",ii);
    } else {
      // TODO: purge everything?
    }

    gettimeofday(&tv, NULL);
    us_diff = 1000000 * (tv_start.tv_sec - tv.tv_sec) +
              (tv_start.tv_usec - tv.tv_usec);

    if ((us_diff / 1000) < time) {
      nanosleep((const struct timespec[]) {{ 0,
                                             (time * 1000 - us_diff) * 1000L } },
                NULL);
    }

    // uint64_t curr_time = time(NULL);
    // if (curr_time - last_purge > 1800) {
    //	friend_cleanup(tox);
    //	save_profile(tox);
    //
    //	last_purge = curr_time;
    // }
  }

  return NULL;
}

/* taken from ToxBot */
static void get_elapsed_time_str(char *buf, int bufsize, uint64_t secs)
{
  long unsigned int minutes = (secs % 3600) / 60;
  long unsigned int hours   = (secs / 3600) % 24;
  long unsigned int days    = (secs / 3600) / 24;

  snprintf(buf, bufsize, "%lud %luh %lum", days, hours, minutes);
}

bool file_exists(const char *filename)
{
  return access(filename, 0) != -1;
}

bool load_profile(Tox **tox, struct Tox_Options *options)
{
  FILE *file = fopen(data_filename, "rb");

  if (file) {
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    uint8_t *save_data = (uint8_t *)malloc(file_size * sizeof(uint8_t));
    fread(save_data, sizeof(uint8_t), file_size, file);
    fclose(file);

    options->savedata_data   = save_data;
    options->savedata_type   = TOX_SAVEDATA_TYPE_TOX_SAVE;
    options->savedata_length = file_size;

    TOX_ERR_NEW err;
    *tox = tox_new(options, &err);
    free(save_data);

    return err == TOX_ERR_NEW_OK;
  }

  return false;
}

uint32_t get_online_friend_count(Tox *tox)
{
  uint32_t online_friend_count = 0u;
  uint32_t friend_count        = tox_self_get_friend_list_size(tox);
  uint32_t friends[friend_count];

  tox_self_get_friend_list(tox, friends);

  for (uint32_t i = 0; i < friend_count; i++) {
    if (tox_friend_get_connection_status(tox, friends[i],
                                         NULL) != TOX_CONNECTION_NONE) {
      online_friend_count++;
    }
  }

  return online_friend_count;
}

void self_connection_status(Tox *tox, TOX_CONNECTION status, void *userData)
{
  if (status == TOX_CONNECTION_NONE) {
    printf("Lost connection to the tox network\n");
    is_connected = false;
  } else {
    printf("Connected to the tox network, status: %d\n", status);
    is_connected = true;
  }
}

void friend_request(Tox           *tox,
                    const uint8_t *public_key,
                    const uint8_t *message,
                    size_t         length,
                    void          *user_data)
{
  TOX_ERR_FRIEND_ADD err;

  tox_friend_add_norequest(tox, public_key, &err);

  if (err != TOX_ERR_FRIEND_ADD_OK) {
    printf("Could not add friend, error: %d\n", err);
  } else {
    printf("Added to our friend list\n");
  }

  save_profile(tox);
}

void custom_lossy_friend_message(Tox           *tox,
                                 uint32_t       friend_number,
                                 const uint8_t *data,
                                 size_t         length,
                                 void          *user_data)
{
  friend *f =  get_friend(friend_number, true);

  if (f == NULL) {
    printf("Hit max transfer limit! Packets will be dropped\n");

    // TODO: notify the user!!
    return;
  } else if ((f->data_packets_cached_bytes + length) >
             MAX_CUSTOM_PACKETS_CACHED_BYTES_PER_USER) {
    printf("Hit max transfer limit! Packets will be dropped\n");

    // TODO: notify the user!!
    return;
  }


  packet_t *pkt = (packet_t *)calloc(1, sizeof(packet_t));
  pkt->data = malloc(sizeof(uint8_t) * length);

  // check allocation for NULL
  memcpy(pkt->data, data, sizeof(uint8_t) * length);
  pkt->lenght                   = length;
  pkt->lossless                 = false;
  f->data_packets_cached_bytes += length;
  DL_APPEND(f->data_packets, pkt);
}

void custom_lossless_friend_message(Tox           *tox,
                                    uint32_t       friend_number,
                                    const uint8_t *data,
                                    size_t         length,
                                    void          *user_data)
{
  friend *f = get_friend(friend_number, true);

  if (f == NULL) {
    printf("Hit transfer limit! Packets will be dropped\n");

    // TODO: notify the user!!
    return;
  } else if ((f->data_packets_cached_bytes + length) >
             MAX_CUSTOM_PACKETS_CACHED_BYTES_PER_USER) {
    printf("Hit max transfer limit! Packets will be dropped\n");

    // TODO: notify the user!!
    return;
  }

  packet_t *pkt = (packet_t *)calloc(1, sizeof(packet_t));
  pkt->data = malloc(sizeof(uint8_t) * length);

  // check allocation for NULL
  memcpy(pkt->data, data, sizeof(uint8_t) * length);
  pkt->lenght                   = length;
  pkt->lossless                 = true;
  f->data_packets_cached_bytes += length;
  DL_APPEND(f->data_packets, pkt);
}

void friend_message(Tox             *tox,
                    uint32_t         friend_number,
                    TOX_MESSAGE_TYPE type,
                    const uint8_t   *message,
                    size_t           length,
                    void            *user_data)
{
  char dest_msg[length + 1];

  dest_msg[length] = '\0';
  memcpy(dest_msg, message, length);

  if (!strcmp("!info", dest_msg)) {
    char time_msg[TOX_MAX_MESSAGE_LENGTH];
    char time_str[64];
    uint64_t cur_time = time(NULL);

    get_elapsed_time_str(time_str, sizeof(time_str), cur_time - start_time);
    snprintf(time_msg, sizeof(time_msg), "Uptime: %s", time_str);
    tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL,
                            (uint8_t *)time_msg, strlen(time_msg), NULL);

    char friend_msg[100];
    snprintf(friend_msg,
             sizeof(friend_msg),
             "Friends: %zu (%d online)",
             tox_self_get_friend_list_size(tox),
             get_online_friend_count(tox));
    tox_friend_send_message(tox,
                            friend_number,
                            TOX_MESSAGE_TYPE_NORMAL,
                            (uint8_t *)friend_msg,
                            strlen(friend_msg),
                            NULL);

    const char *friend_info_msg =
      "Friends are removed after 1 month of inactivity";
    tox_friend_send_message(tox,
                            friend_number,
                            TOX_MESSAGE_TYPE_NORMAL,
                            (uint8_t *)friend_info_msg,
                            strlen(friend_info_msg),
                            NULL);

    const char *info_msg =
      "If you're experiencing issues, contact bonfus in #toktok at freenode";
    tox_friend_send_message(tox,
                            friend_number,
                            TOX_MESSAGE_TYPE_NORMAL,
                            (uint8_t *)info_msg,
                            strlen(info_msg),
                            NULL);
  } else if (!strcmp("!callme", dest_msg)) {
    toxav_call(g_toxAV, friend_number, audio_bitrate, 0, NULL);
  } else if (!strcmp("!videocallme", dest_msg)) {
    toxav_call(g_toxAV, friend_number, audio_bitrate, video_bitrate, NULL);
  } else {
    /* Just repeat what has been said like the nymph Echo. */
    tox_friend_send_message(tox,
                            friend_number,
                            TOX_MESSAGE_TYPE_NORMAL,
                            message,
                            length,
                            NULL);

    /* Send usage instructions in new message. */
    static const char *help_msg =
      "TestBot commands:\n!info: Show stats.\n!callme: Launch an audio call.\n!videocallme: Launch a video call.";
    tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL,
                            (uint8_t *)help_msg, strlen(help_msg), NULL);
  }
}

file_transfer_t* init_file_transfer(uint32_t       file_number,
                                    uint8_t       *file_id,
                                    uint32_t       kind,
                                    uint64_t       file_size,
                                    const uint8_t *filename,
                                    size_t         filename_length) {
  file_transfer_t *ft = malloc(sizeof(file_transfer_t));

  ft->file_number = file_number;
  ft->kind        = kind;
  ft->file_size   = file_size;
  ft->file_id     = file_id;
  ft->filename    = (uint8_t *)malloc(sizeof(uint8_t) * filename_length);
  memcpy(ft->filename, filename, filename_length);
  ft->filename_length = filename_length;
  ft->recv_complete   = false;
  ft->send_started    = false;
  ft->send_complete   = false;
  ft->is_error        = false;
  ft->last_recv_time  = INT64_MAX; // in the future means never
  ft->last_send_time  = INT64_MAX;
  ft->sent_pos        = 0;
  ft->data            = calloc(file_size, sizeof(uint8_t));
  return ft;
}

void file_recv(Tox           *tox,
               uint32_t       friend_number,
               uint32_t       file_number,
               uint32_t       kind,
               uint64_t       file_size,
               const uint8_t *filename,
               size_t         filename_length,
               void          *user_data)
{
  if (kind == TOX_FILE_KIND_AVATAR) {
    return;
  }

  if (file_size > MAX_FILE_TRANSFER_SIZE) {
    tox_file_control(tox,
                     friend_number,
                     file_number,
                     TOX_FILE_CONTROL_CANCEL,
                     NULL);

    const char *tmp =
      "Sorry, I don't accept files larger than %d bytes (and streaming not yet supported)";
    char *msg = (char *)malloc(sizeof(char) * 1000);
    sprintf(msg, tmp, MAX_FILE_TRANSFER_SIZE);
    tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL,
                            (uint8_t *)msg, strlen(msg), NULL);
    free(msg);
  } else {
    // look for friend
    friend *f;
    file_transfer_t *ft;
    int ntransfers = 0;

    f = get_friend(friend_number, false);

    if (f != NULL) {
      DL_COUNT(f->file_transfers, ft, ntransfers);

      if (ntransfers > MAX_FILE_TRANSFERS_PER_USER) {
        tox_file_control(tox,
                         friend_number,
                         file_number,
                         TOX_FILE_CONTROL_CANCEL,
                         NULL);

        const char *tmp = "Sorry, no more than %d files per user.";
        char *msg       = (char *)malloc(sizeof(char) * 1000);
        sprintf(msg, tmp, MAX_FILE_TRANSFERS_PER_USER);
        tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL,
                                (uint8_t *)msg, strlen(msg), NULL);
        free(msg);
        return;
      }
    }

    f = get_friend(friend_number, true);

    if (f == NULL) {
      printf("Hit transfer limit! Packets will be dropped\n");
      const char *tmp =
        "Hit server transfer limit (%d transfers)! File transfer will be dropped";
      char *msg = (char *)malloc(sizeof(char) * 1000);
      sprintf(msg, tmp, MAX_TOTAL_TRANSFERS);
      tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL,
                              (uint8_t *)msg, strlen(msg), NULL);
      free(msg);
      return;
    }

    // get file id
    uint8_t *file_id = malloc(sizeof(uint8_t) * TOX_FILE_ID_LENGTH);
    TOX_ERR_FILE_GET error;
    tox_file_get_file_id(tox, friend_number, file_number, file_id, &error);

    ft = init_file_transfer(file_number,
                            file_id,
                            kind,
                            file_size,
                            filename,
                            filename_length);
    DL_APPEND(f->file_transfers, ft);
    tox_file_control(tox,
                     friend_number,
                     file_number,
                     TOX_FILE_CONTROL_RESUME,
                     NULL);
  }
}

void file_recv_chunk(Tox           *tox,
                     uint32_t       friend_number,
                     uint32_t       file_number,
                     uint64_t       position,
                     const uint8_t *data,
                     size_t         length,
                     void          *user_data)
{
  friend *f = get_friend(friend_number, false);

  printf("receiving data\n");

  if (f != NULL) {
    file_transfer_t *el;
    DL_FOREACH(f->file_transfers, el) {
      if (el->file_number == file_number) {
        if (length == 0) { // transfer finished
          el->recv_complete  = true;
          el->last_recv_time = monotonic_time_in_ms();
          printf("File transfer completed!\n");
          break;
        }

        // this should never happen
        if (el->data != NULL) {
          if (position < el->file_size) {
            memcpy(el->data + position, data, length);
            el->last_recv_time = monotonic_time_in_ms();
          } else {
            printf("Invalid position\n");
            el->is_error = true;
          }
        }
        break;
      }

      // file transfer not allowed, send CANCEL
      // TODO
    }
  } else {
    printf("Could not find file transfer for friend %d\n", friend_number);
  }
}

void file_recv_control(Tox *tox, uint32_t friend_number, uint32_t
                       file_number, TOX_FILE_CONTROL control, void *user_data) {
  // we do not support pause yet
  if ((control == TOX_FILE_CONTROL_PAUSE) ||
      (control == TOX_FILE_CONTROL_RESUME)) {
    tox_file_control(tox,
                     friend_number,
                     file_number,
                     TOX_FILE_CONTROL_RESUME,
                     NULL);

    // const char *msg = "Sorry, I don't support pause (yet)." ;
    // tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL,
    // (uint8_t*)msg, strlen(msg), NULL);
    return;
  }

  // IF not TOX_FILE_CONTROL_PAUSE || control == TOX_FILE_CONTROL_RESUME
  // then it's TOX_FILE_CONTROL_CANCEL
  friend *f = get_friend(friend_number, false);

  if (f != NULL) {
    file_transfer_t *el, *tmp;
    DL_FOREACH_SAFE(f->file_transfers, el, tmp) {
      if (el->file_number == file_number) {
        DL_DELETE(f->file_transfers, el);
        clean_file_transfer(el);
      }
    }
  }
}

void call(ToxAV   *toxAV,
          uint32_t friend_number,
          bool     audio_enabled,
          bool     video_enabled,
          void    *user_data)
{
  TOXAV_ERR_ANSWER err;

  toxav_answer(toxAV,
               friend_number,
               audio_enabled ? audio_bitrate : 0,
               video_enabled ? video_bitrate : 0,
               &err);

  if (err != TOXAV_ERR_ANSWER_OK) {
    printf("Could not answer call, friend: %d, error: %d\n", friend_number, err);
  }
}

void call_state(ToxAV   *toxAV,
                uint32_t friend_number,
                uint32_t state,
                void    *user_data)
{
  if (state & TOXAV_FRIEND_CALL_STATE_FINISHED) {
    printf("Call with friend %d finished\n", friend_number);
    return;
  } else if (state & TOXAV_FRIEND_CALL_STATE_ERROR) {
    printf("Call with friend %d errored\n", friend_number);
    return;
  }

  bool send_audio = (state & TOXAV_FRIEND_CALL_STATE_SENDING_A) &&
                    (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_A);
  bool send_video = state & TOXAV_FRIEND_CALL_STATE_SENDING_V &&
                    (state & TOXAV_FRIEND_CALL_STATE_ACCEPTING_V);
  toxav_audio_set_bit_rate(toxAV,
                           friend_number,
                           send_audio ? audio_bitrate : 0,
                           NULL);
  toxav_video_set_bit_rate(toxAV,
                           friend_number,
                           send_video ? video_bitrate : 0,
                           NULL);

  printf("Call state for friend %d changed to %d: audio: %d, video: %d\n",
         friend_number,
         state,
         send_audio,
         send_video);
}

void audio_receive_frame(ToxAV         *toxAV,
                         uint32_t       friend_number,
                         const int16_t *pcm,
                         size_t         sample_count,
                         uint8_t        channels,
                         uint32_t       sampling_rate,
                         void          *user_data)
{
  TOXAV_ERR_SEND_FRAME err;

  toxav_audio_send_frame(toxAV,
                         friend_number,
                         pcm,
                         sample_count,
                         channels,
                         sampling_rate,
                         &err);

  if (err != TOXAV_ERR_SEND_FRAME_OK) {
    printf("Could not send audio frame to friend: %d, error: %d\n",
           friend_number,
           err);
  }
}

void video_receive_frame(ToxAV         *toxAV,
                         uint32_t       friend_number,
                         uint16_t       width,
                         uint16_t       height,
                         const uint8_t *y,
                         const uint8_t *u,
                         const uint8_t *v,
                         int32_t        ystride,
                         int32_t        ustride,
                         int32_t        vstride,
                         void          *user_data)
{
  ystride = abs(ystride);
  ustride = abs(ustride);
  vstride = abs(vstride);

  if ((ystride < width) || (ustride < width / 2) || (vstride < width / 2)) {
    printf("wtf\n");
    return;
  }

  uint8_t *y_dest = (uint8_t *)malloc(width * height);
  uint8_t *u_dest = (uint8_t *)malloc(width * height / 2);
  uint8_t *v_dest = (uint8_t *)malloc(width * height / 2);

  for (size_t h = 0; h < height; h++) {
    memcpy(&y_dest[h * width], &y[h * ystride], width);
  }

  for (size_t h = 0; h < height / 2; h++) {
    memcpy(&u_dest[h * width / 2], &u[h * ustride], width / 2);
    memcpy(&v_dest[h * width / 2], &v[h * vstride], width / 2);
  }

  TOXAV_ERR_SEND_FRAME err;
  toxav_video_send_frame(toxAV,
                         friend_number,
                         width,
                         height,
                         y_dest,
                         u_dest,
                         v_dest,
                         &err);

  free(y_dest);
  free(u_dest);
  free(v_dest);

  if (err != TOXAV_ERR_SEND_FRAME_OK) {
    printf("Could not send video frame to friend: %d, error: %d\n",
           friend_number,
           err);
  }
}

static void handle_signal(int sig)
{
  signal_exit = true;
}

static void print_log(Tox          *tox,
                      TOX_LOG_LEVEL level,
                      const char   *file,
                      uint32_t      line,
                      const char   *func,
                      const char   *message,
                      void         *user_data)
{
  if (level >= TOX_LOG_LEVEL_WARNING) printf(
      "LOG MESSAGE: func: %s message: %s\n",
      func,
      message);
}

int main(int argc, char *argv[])
{
  signal(SIGINT, handle_signal);
  start_time = time(NULL);

  TOX_ERR_NEW err = TOX_ERR_NEW_OK;
  struct Tox_Options options;
  tox_options_default(&options);
  tox_options_set_log_callback(&options, print_log);

  if (file_exists(data_filename)) {
    if (load_profile(&g_tox, &options)) {
      printf("Loaded data from disk\n");
    } else {
      printf("Failed to load data from disk\n");
      return -1;
    }
  } else {
    printf("Creating a new profile\n");

    g_tox = tox_new(&options, &err);
    save_profile(g_tox);
  }

  tox_callback_self_connection_status(g_tox, self_connection_status);
  tox_callback_friend_request(g_tox, friend_request);
  tox_callback_friend_message(g_tox, friend_message);
  tox_callback_friend_lossy_packet(g_tox, custom_lossy_friend_message);
  tox_callback_friend_lossless_packet(g_tox, custom_lossless_friend_message);
  tox_callback_file_recv(g_tox, file_recv);
  tox_callback_file_recv_chunk(g_tox, file_recv_chunk);
  tox_callback_file_recv_control(g_tox, file_recv_control);
  tox_callback_file_chunk_request(g_tox, file_chunk_request);

  if (err != TOX_ERR_NEW_OK) {
    printf("Error at tox_new, error: %d\n", err);
    return -1;
  }

  uint8_t address_bin[TOX_ADDRESS_SIZE];
  tox_self_get_address(g_tox, (uint8_t *)address_bin);
  char address_hex[TOX_ADDRESS_SIZE * 2 + 1];
  sodium_bin2hex(address_hex,
                 sizeof(address_hex),
                 address_bin,
                 sizeof(address_bin));

  printf("%s\n",
         address_hex);
  printf(
    "At maximum load, echobot will approximately use %d megabytes in memory\n",
         MAX_TOTAL_TRANSFERS *
    (MAX_CUSTOM_PACKETS_CACHED_BYTES_PER_USER + MAX_FILE_TRANSFERS_PER_USER *
     MAX_FILE_TRANSFER_SIZE) / 1024);

  const char *name       = "TestBot";
  const char *status_msg =
    "Tox audio/video file transfer and custom packet testing service. Send '!info' for stats.";

  tox_self_set_name(g_tox, (uint8_t *)name, strlen(name), NULL);
  tox_self_set_status_message(g_tox,
                              (uint8_t *)status_msg,
                              strlen(status_msg),
                              NULL);

  const char *key_hex =
    "F404ABAA1C99A9D37D61AB54898F56793E1DEF8BD46B1038B9D822E8460FAB67";
  uint8_t key_bin[TOX_PUBLIC_KEY_SIZE];
  sodium_hex2bin(key_bin,
                 sizeof(key_bin),
                 key_hex,
                 strlen(key_hex),
                 NULL,
                 NULL,
                 NULL);

  TOX_ERR_BOOTSTRAP err3;
  tox_bootstrap(g_tox, "node.tox.biribiri.org", 33445, key_bin, &err3);

  if (err3 != TOX_ERR_BOOTSTRAP_OK) {
    printf("Could not bootstrap, error: %d\n", err3);
    return -1;
  }

  TOXAV_ERR_NEW err2;
  g_toxAV = toxav_new(g_tox, &err2);
  toxav_callback_call(g_toxAV, call, NULL);
  toxav_callback_call_state(g_toxAV, call_state, NULL);
  toxav_callback_audio_receive_frame(g_toxAV, audio_receive_frame, NULL);
  toxav_callback_video_receive_frame(g_toxAV, video_receive_frame, NULL);

  if (err2 != TOXAV_ERR_NEW_OK) {
    printf("Error at toxav_new: %d\n", err);
    return -1;
  }

  pthread_t tox_thread, toxav_thread;
  pthread_create(&tox_thread,   NULL, &run_tox,   g_tox);
  pthread_create(&toxav_thread, NULL, &run_toxav, g_toxAV);

  while (!signal_exit) {
    nanosleep((const struct timespec[]) {{ 0, 500000000L } }, NULL);
  }

  printf("Killing tox and saving profile\n");

  pthread_cancel(tox_thread);
  pthread_cancel(toxav_thread);

  friend *elt, *tmp1, *tmp2;
  CDL_FOREACH_SAFE(data_from_frnds, elt, tmp1, tmp2) {
    purge_packets(elt);
    purge_file_transfers(elt);
    CDL_DELETE(data_from_frnds, elt);
    free(elt);
  }

  save_profile(g_tox);
  toxav_kill(g_toxAV);
  tox_kill(g_tox);

  return 0;
}
