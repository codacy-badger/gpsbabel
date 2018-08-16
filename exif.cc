/*

    Support for embedded (JPEG) Exif-GPS information.

    Copyright (C) 2008 Olaf Klein, o.b.klein@gpsbabel.org

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111 USA

 */

/*
 * Exif specifications can be found at
 * 2016, version 2.31: http://www.cipa.jp/std/documents/e/DC-008-Translation-2016-E.pdf
 * 2012, version 2.3: http://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf
 * 2010, version 2.3: http://www.cipa.jp/std/documents/e/DC-008-2010_E.pdf
 * 2002, version 2.2: http://www.exif.org/Exif2-2.PDF
 * 1998, version 2.1: http://www.exif.org/Exif2-1.PDF
 *
 * TIFF specifications can be found at
 * version 6.0: https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf
 * version 6.0: http://www.npes.org/pdf/TIFF-v6.pdf
 * version 6.0: http://www.alternatiff.com/resources/TIFF6.pdf
 */

#include "defs.h"
#include "garmin_tables.h"         // for gt_lookup_datum_index
#include "gbfile.h"                // for gbfile, gbfclose, gbfcopyfrom, gbfseek, gbfwrite, gbfopen_be, gbftell, gbfputuint16, gbfputuint32, gbfgetuint16, gbfgetuint32, gbfread, gbfrewind, gbfgetflt
#include "jeeps/gpsmath.h"         // for GPS_Math_WGS84_To_Known_Datum_M
#include "src/core/datetime.h"     // for DateTime
#include <QtCore/QByteArray>       // for QByteArray
#include <QtCore/QDateTime>        // for QDateTime
#include <QtCore/QFile>            // for QFile
#include <QtCore/QList>            // for QList<>::iterator, QList
#include <QtCore/QString>          // for QString
#include <QtCore/QTextCodec>       // for QTextCodec
#include <QtCore/QtGlobal>         // for qPrintable
#include <algorithm>               // for sort
#include <cassert>                 // for assert
#include <cmath>                   // for fabs, floor
#include <cstdio>                  // for snprintf, SEEK_SET, sscanf
#include <cstdint>                 // for int32_t, int16_t, uint16_t, uint32_t, uint8_t
#include <cstdlib>                 // for labs, atoi, abs
#include <cstring>                 // for strrchr, memcmp, memcpy, memset, strchr, strlen, strncat
#include <ctime>                   // for gmtime, localtime, time_t, tm

#define MYNAME "exif"

// #define EXIF_DBG

#define UNKNOWN_TIMESTAMP 999999999

#define IFD0    0
#define IFD1    1
#define EXIF_IFD  2   /* dummy index */
#define GPS_IFD   3   /* dummy index */
#define INTER_IFD 4   /* dummy index */

#define EXIF_TYPE_BYTE    1
#define EXIF_TYPE_ASCII   2
#define EXIF_TYPE_SHORT   3
#define EXIF_TYPE_LONG    4
#define EXIF_TYPE_RAT     5
#define EXIF_TYPE_SBYTE   6   /* TIFF 6.0 */
#define EXIF_TYPE_UNK     7   /* TIFF 6.0 */
#define EXIF_TYPE_SSHORT  8   /* TIFF 6.0 */
#define EXIF_TYPE_SLONG   9   /* TIFF 6.0 */
#define EXIF_TYPE_SRAT    10  /* TIFF 6.0 */
#define EXIF_TYPE_FLOAT   11  /* TIFF 6.0 */
#define EXIF_TYPE_DOUBLE  12  /* TIFF 6.0 */
#define EXIF_TYPE_IFD     13
#define EXIF_TYPE_UNICODE 14
#define EXIF_TYPE_COMPLEX 15
#define EXIF_TYPE_LONG8   16  /* BigTIFF */
#define EXIF_TYPE_SLONG8  17  /* BigTIFF */
#define EXIF_TYPE_IFD8    18  /* BigTIFF */

#define BYTE_TYPE(a) ( (a==EXIF_TYPE_BYTE) || (a==EXIF_TYPE_ASCII) || (a==EXIF_TYPE_SBYTE) || (a==EXIF_TYPE_UNK) )
#define WORD_TYPE(a) ( (a==EXIF_TYPE_SHORT) || (a==EXIF_TYPE_SSHORT) )
#define LONG_TYPE(a) ( (a==EXIF_TYPE_LONG) || (a==EXIF_TYPE_SLONG) || (a==EXIF_TYPE_IFD) )

#define IFD0_TAG_EXIF_IFD_OFFS  0x8769 // aka JPEGInterchangeFormat
#define IFD0_TAG_GPS_IFD_OFFS   0x8825  // aka JPEGInterchangeFormatLength

#define IFD1_TAG_STRIP_OFFS   0x0111
#define IFD1_TAG_JPEG_OFFS    0x0201
#define IFD1_TAG_JPEG_SIZE    0x0202

#define EXIF_IFD_TAG_USER_CMT       0x9286
#define EXIF_IFD_TAG_INTER_IFD_OFFS 0xA005

#define GPS_IFD_TAG_VERSION   0x0000
#define GPS_IFD_TAG_LATREF    0x0001
#define GPS_IFD_TAG_LAT       0x0002
#define GPS_IFD_TAG_LONREF    0x0003
#define GPS_IFD_TAG_LON       0x0004
#define GPS_IFD_TAG_ALTREF    0x0005
#define GPS_IFD_TAG_ALT       0x0006
#define GPS_IFD_TAG_TIMESTAMP 0x0007
#define GPS_IFD_TAG_SAT       0x0008
#define GPS_IFD_TAG_MODE      0x000A
#define GPS_IFD_TAG_DOP       0x000B
#define GPS_IFD_TAG_SPEEDREF  0x000C
#define GPS_IFD_TAG_SPEED     0x000D
#define GPS_IFD_TAG_DATUM     0x0012
#define GPS_IFD_TAG_DATESTAMP 0x001D

struct ExifTag {
  uint16_t id{0};           // tag that identifieds the field.
  uint16_t type{0};         // field type.
  uint32_t count{0};        // number of values. Note that Count is not the total number of bytes.
  uint32_t offset{0};       // byte offset relative to beginning of TIFF file to value (only for values longer than 4 bytes).
  uint32_t original{0};
  uint32_t size{0};         // derived size in bytes of value.
#ifdef EXIF_DBG
  uint32_t tag_offset {0};  // byte offset relative to beginning of TIFF file of this tag.
#endif
  QByteArray data;

  bool operator==(const ExifTag& other) const
  {
    return id == other.id;
  }

  // Return data value interpreted as EXIF_TYPE_LONG.
  // This is most useful when the type is EXIF_TYPE_LONG and the count is one,
  // which occurs for multiple specific tags where we need the value.
  inline uint32_t toLong() const
  {
    return *reinterpret_cast<const uint32_t*>(data.constData());
  }
};

struct ExifIfd {
  uint32_t next_ifd{0};
  uint16_t nr{0};
  uint16_t count{0};
  QList<ExifTag> tags;
};

struct ExifApp {
  uint16_t marker{0};
  gbsize_t len{0};
  gbfile* fcache{nullptr};
  gbfile* fexif{nullptr};
  QList<ExifIfd> ifds;

  ~ExifApp()
  {
    if (fcache) {
      gbfclose(fcache);
    }
    if (fexif) {
      gbfclose(fexif);
    }
  }
};

static gbfile* fin, *fout;
static QList<ExifApp>* exif_apps;
static ExifApp* exif_app;
static const Waypoint* exif_wpt_ref;
static time_t exif_time_ref;
static char exif_success;
static QString exif_fout_name;

static char* opt_filename, *opt_overwrite, *opt_frame, *opt_name;

static uint8_t writer_gps_tag_version[4] = {2, 0, 0, 0};

static arglist_t exif_args[] = {
  { "filename", &opt_filename, "Set waypoint name to source filename", "Y", ARGTYPE_BOOL, ARG_NOMINMAX, nullptr },
  { "frame", &opt_frame, "Time-frame (in seconds)", "10", ARGTYPE_INT, "0", nullptr, nullptr },
  { "name", &opt_name, "Locate waypoint for tagging by this name", nullptr, ARGTYPE_STRING, ARG_NOMINMAX, nullptr },
  { "overwrite", &opt_overwrite, "!OVERWRITE! the original file. Default=N", "N", ARGTYPE_BOOL, ARG_NOMINMAX, nullptr },
  ARG_TERMINATOR
};

#ifdef EXIF_DBG
static void
print_buff(const char* buf, int sz, const char* cmt)
{
  int i;

  printf("%s: ", cmt);
  for (i = 0; i < sz; i++) {
    printf("%02x ", buf[i] & 0xFF);
  }
  for (i = 0; i < sz; i++) {
    char c = buf[i];
    if (isspace(c)) {
      c = ' ';
    } else if (! isprint(c)) {
      c = '.';
    }
    printf("%c", c);
  }
}
#endif

static uint16_t
exif_type_size(const uint16_t type)
{
  uint16_t size;

  switch (type) {
  case EXIF_TYPE_BYTE:
  case EXIF_TYPE_ASCII:
  case EXIF_TYPE_UNK:
  case EXIF_TYPE_SBYTE:
    size = 1;
    break;

  case EXIF_TYPE_SHORT:
  case EXIF_TYPE_SSHORT:
  case EXIF_TYPE_UNICODE:
    size = 2;
    break;
  case EXIF_TYPE_IFD:
  case EXIF_TYPE_LONG:
  case EXIF_TYPE_SLONG:
  case EXIF_TYPE_FLOAT:
    size = 4;
    break;

  case EXIF_TYPE_RAT:
  case EXIF_TYPE_SRAT:
  case EXIF_TYPE_DOUBLE:
  case EXIF_TYPE_COMPLEX:
  case EXIF_TYPE_LONG8:
  case EXIF_TYPE_SLONG8:
  case EXIF_TYPE_IFD8:
    size = 8;
    break;

  default:
    fatal(MYNAME ": Unknown data type %d! Please report.\n", type);
  }
  return size;
}

// TODO: If this were actually ever used (!?!?!) it could probably be
// replaced by return QDateTime(time).toString("yyyy/MM/dd, hh:mm:ss);
static QString
exif_time_str(const time_t time)
{
  struct tm tm = *localtime(&time);
  tm.tm_year += 1900;
  tm.tm_mon += 1;

  return QString().sprintf("%04d/%02d/%02d, %02d:%02d:%02d",
                           tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static char*
exif_read_str(ExifTag* tag)
{
  // Panasonic DMC-TZ10 stores datum with trailing spaces.
  char* buf = xstrndup(tag->data.constData(), tag->size);
  rtrim(buf);
  return buf;
}

static double
exif_read_double(const ExifTag* tag, const int index)
{
  int32_t* data = (int32_t*)tag->data.constData();

  unsigned int num = data[index * 2];
  unsigned int den = data[(index * 2) + 1];
  if (den == 0) {
    den = 1;
  }

  return (double)num / (double)den;
}

static double
exif_read_coord(const ExifTag* tag)
{
  double res = exif_read_double(tag, 0);
  if (tag->count == 1) {
    return res;
  }

  double min = exif_read_double(tag, 1);
  res += (min / 60);
  if (tag->count == 2) {
    return res;
  }

  double sec = exif_read_double(tag, 2);
  res += (sec / 3600);

  return res;
}

static time_t
exif_read_timestamp(const ExifTag* tag)
{
  double hour = exif_read_double(tag, 0);
  double min = exif_read_double(tag, 1);
  double sec = exif_read_double(tag, 2);

  return ((int)hour * SECONDS_PER_HOUR) + ((int)min * 60) + (int)sec;
}

static time_t
exif_read_datestamp(const ExifTag* tag)
{
  struct tm tm;

  memset(&tm, 0, sizeof(tm));
  char* str = xstrndup(tag->data.constData(), tag->size);
  sscanf(str, "%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
  xfree(str);

  tm.tm_year -= 1900;
  tm.tm_mon -= 1;

  return mkgmtime(&tm);
}

static void
exif_release_apps()
{
  delete exif_apps;
  exif_apps = nullptr;
}

static uint32_t
exif_ifd_size(ExifIfd* ifd)
{
  uint32_t res = 6;   /* nr of tags + next_ifd */

  res += (ifd->count * 12);
  for (auto& tag_instance : ifd->tags) {
    ExifTag* tag = &tag_instance;
    if (tag->size > 4) {
      uint32_t size = tag->size;
      if (size & 1u) {
        size++;
      }
      res += size;
    }
  }

  return res;
}

static ExifApp*
exif_load_apps()
{
  ExifApp* exif_app = nullptr;

  while (! gbfeof(fin)) {
    exif_apps->append(ExifApp());
    ExifApp* app = &exif_apps->last();
    app->fcache = gbfopen(nullptr, "wb", MYNAME);

    app->marker = gbfgetuint16(fin);
    app->len = gbfgetuint16(fin);
#ifdef EXIF_DBG
    printf(MYNAME ": api = %02X, len = %u (0x%04x), offs = 0x%08X\n", app->marker & 0xFF, app->len, app->len, gbftell(fin));
#endif
    if (exif_app || (app->marker == 0xFFDA)) /* compressed data */ {
      gbfcopyfrom(app->fcache, fin, 0x7FFFFFFF);
#ifdef EXIF_DBG
      printf(MYNAME ": compressed data size = %d\n", gbftell(app->fcache));
#endif
    } else {
      gbfcopyfrom(app->fcache, fin, app->len - 2);
      if (app->marker == 0xFFE1) {
        exif_app = app;
      }
    }
  }

  return exif_app;
}

static ExifIfd*
exif_read_ifd(ExifApp* app, const uint16_t ifd_nr, const gbsize_t offs,
              uint32_t* exif_ifd_ofs, uint32_t* gps_ifd_ofs, uint32_t* inter_ifd_ofs)
{
  gbfile* fin = app->fexif;

  app->ifds.append(ExifIfd());
  ExifIfd* ifd = &app->ifds.last();
  ifd->nr = ifd_nr;

  gbfseek(fin, offs, SEEK_SET);
  ifd->count = gbfgetuint16(fin);

#ifdef EXIF_DBG
  {
    const char* name;
    switch (ifd_nr) {
    case IFD0:
      name = "IFD0";
      break;
    case IFD1:
      name = "IFD1";
      break;
    case GPS_IFD:
      name = "GPS";
      break;
    case EXIF_IFD:
      name = "EXIF";
      break;
    case INTER_IFD:
      name = "INTER";
      break;
    default:
      name = "private";
      break;
    }
    printf(MYNAME "-offs 0x%08X: Number of items in IFD%d \"%s\" = %d (0x%04x)\n",
           offs, ifd_nr, name, ifd->count, ifd->count);
  }
#endif
  if (ifd->count == 0) {
    return ifd;
  }

  for (uint16_t i = 0; i < ifd->count; i++) {
    ifd->tags.append(ExifTag());
    ExifTag* tag = &ifd->tags.last();
#ifdef EXIF_DBG
    tag->tag_offset = gbftell(fin);
#endif

    tag->id = gbfgetuint16(fin);
    tag->type = gbfgetuint16(fin);
    tag->count = gbfgetuint32(fin);
    tag->size = exif_type_size(tag->type) * tag->count;
    tag->data = QByteArray(4, 0);

    if (tag->size <= 4) { // data is in value offset field
      if (BYTE_TYPE(tag->type)) {
        gbfread(tag->data.data(), 4, 1, fin);
      } else if (WORD_TYPE(tag->type)) {
        uint16_t* ptr = reinterpret_cast<uint16_t*>(tag->data.data());
        *ptr = gbfgetuint16(fin);
        *(ptr+1) = gbfgetuint16(fin);
      } else if (LONG_TYPE(tag->type)) {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(tag->data.data());
        *ptr = gbfgetuint32(fin);
      } else if (tag->type == EXIF_TYPE_FLOAT) {
        float* ptr = reinterpret_cast<float*>(tag->data.data());
        *ptr = gbfgetflt(fin);
      } else {
        fatal(MYNAME "Unknown type %d has size <= 4! Please report.\n", tag->type);
      }
      tag->original = tag->toLong();
    } else { // offset is in value offset field
      tag->offset = gbfgetuint32(fin);
      tag->original = tag->offset;
    }

    if (ifd_nr == IFD0) {
      if (tag->id == IFD0_TAG_EXIF_IFD_OFFS) {
        *exif_ifd_ofs = tag->toLong();
      } else if (tag->id == IFD0_TAG_GPS_IFD_OFFS) {
        *gps_ifd_ofs = tag->toLong();
      }
    } else if (ifd_nr == EXIF_IFD) {
      if (tag->id == EXIF_IFD_TAG_INTER_IFD_OFFS) {
        *inter_ifd_ofs = tag->toLong();
      }
    }
  }

#ifdef EXIF_DBG
  gbsize_t next_ifd_offs = gbftell(fin);
#endif
  ifd->next_ifd = gbfgetuint32(fin);

  for (auto& tag_instance : ifd->tags) {
    ExifTag* tag = &tag_instance;
    if ((tag->size > 4) && (tag->offset)) {
      tag->data = QByteArray(tag->size, 0);

      char* ptr = tag->data.data();
      gbfseek(fin, tag->offset, SEEK_SET);

      if (BYTE_TYPE(tag->type)) {
        gbfread(ptr, tag->count, 1, fin);
      } else for (uint16_t i = 0; i < tag->count; i++) {
          switch (tag->type) {
          case EXIF_TYPE_SHORT:
          case EXIF_TYPE_SSHORT:
            *(int16_t*)ptr = gbfgetuint16(fin);
            break;
          case EXIF_TYPE_IFD:
          case EXIF_TYPE_LONG:
          case EXIF_TYPE_SLONG:
            *(int32_t*)ptr = gbfgetuint32(fin);
            break;
          case EXIF_TYPE_RAT:
          case EXIF_TYPE_SRAT:
            *(int32_t*)ptr = gbfgetuint32(fin);
            *(int32_t*)(ptr+4) = gbfgetuint32(fin);
            break;
          case EXIF_TYPE_FLOAT:
            *(float*)ptr = gbfgetflt(fin);
            break;
          case EXIF_TYPE_DOUBLE:
            *(double*)ptr = gbfgetdbl(fin);
            break;
          default:
            gbfread(ptr, exif_type_size(tag->type), 1, fin);
            break;
          }
          ptr += (tag->size / tag->count);
        }
    }
#ifdef EXIF_DBG
    printf(MYNAME "-offs 0x%08X: ifd=%d id=0x%04X t=0x%04X c=%4u s=%4u",
           tag->tag_offset, ifd->nr, tag->id, tag->type, tag->count, tag->size);
    if (tag->size > 4) {
      printf(" o=0x%08X", tag->offset);
    } else {
      printf(" v=0x%08X", tag->toLong());
    }
    if (tag->type == EXIF_TYPE_ASCII) {
      char* str = exif_read_str(tag);
      printf(" \"%s\"", str);
      xfree(str);
    }
    printf("\n");
#endif
  }

#ifdef EXIF_DBG
  printf(MYNAME "-offs 0x%08X: Next IFD=0x%08X\n", next_ifd_offs,  ifd->next_ifd);
#endif

  return ifd;
}

static void
exif_read_app(ExifApp* app)
{
  gbsize_t offs;
  uint32_t exif_ifd_ofs, gps_ifd_ofs, inter_ifd_ofs;
  ExifIfd* ifd;
  gbfile* fin = app->fexif;

#ifdef EXIF_DBG
  printf(MYNAME ": read_app...\n");
  print_buff((const char*)fin->handle.mem, 8, MYNAME "-offs 0x00000000: Image File Header");
  printf("\n");
#endif
  exif_ifd_ofs = gps_ifd_ofs = inter_ifd_ofs = 0;

  gbfseek(fin, 4, SEEK_SET);
  offs = gbfgetuint32(fin); // Image File Header Bytes 4-7, the offset (in bytes) of the first IFD.

  ifd = exif_read_ifd(app, IFD0, offs, &exif_ifd_ofs, &gps_ifd_ofs, &inter_ifd_ofs);
  if (ifd == nullptr) {
    return;
  }
  if (ifd->next_ifd) {
    ifd = exif_read_ifd(app, IFD1, ifd->next_ifd, &exif_ifd_ofs, &gps_ifd_ofs, &inter_ifd_ofs);
  }
  if (exif_ifd_ofs) {
    ifd = exif_read_ifd(app, EXIF_IFD, exif_ifd_ofs, nullptr, nullptr, &inter_ifd_ofs);
  }
  if (gps_ifd_ofs) {
    ifd = exif_read_ifd(app, 3, gps_ifd_ofs, nullptr, nullptr, nullptr);
  }
  if (inter_ifd_ofs) {
    ifd = exif_read_ifd(app, 4, inter_ifd_ofs, nullptr, nullptr, nullptr);
  }
  // The return values of exif_read_ifd above aren't actually used.
  // Warning hush.
  (void) ifd;
}

static void
exif_examine_app(ExifApp* app)
{
  gbfile* ftmp = app->fcache;

  gbfrewind(ftmp);
  uint32_t ident = gbfgetuint32(ftmp);
  is_fatal(ident != 0x66697845, MYNAME ": Invalid EXIF header magic.");
  is_fatal(gbfgetint16(ftmp) != 0, MYNAME ": Error in EXIF header.");
  uint16_t endianess = gbfgetint16(ftmp);

#ifdef EXIF_DBG
  printf(MYNAME ": endianess = 0x%04X\n", endianess);
#endif
  if (endianess == 0x4949) {
    ftmp->big_endian = 0;
  } else if (endianess == 0x4D4D) {
    ftmp->big_endian = 1;
  } else {
    fatal(MYNAME ": Invalid endianess identifier 0x%04X!\n", endianess);
  }

  gbfseek(ftmp, 6, SEEK_SET);
  app->fexif = gbfopen(nullptr, "wb", MYNAME);
  app->fexif->big_endian = ftmp->big_endian;
  gbfcopyfrom(app->fexif, ftmp, 0x7FFFFFFF);

  exif_read_app(app);
}

static ExifIfd*
exif_find_ifd(ExifApp* app, const uint16_t ifd_nr)
{
  for (auto& ifd_instance : app->ifds) {
    ExifIfd* ifd = &ifd_instance;

    if (ifd->nr == ifd_nr) {
      return ifd;
    }
  }
  return nullptr;
}

static ExifTag*
exif_find_tag(ExifApp* app, const uint16_t ifd_nr, const uint16_t tag_id)
{
  ExifIfd* ifd = exif_find_ifd(app, ifd_nr);
  if (ifd != nullptr) {
    for (auto& tag_instance : ifd->tags) {
      ExifTag* tag = &tag_instance;
      if (tag->id == tag_id) {
        return tag;
      }
    }
  }
  return nullptr;
}

static time_t
exif_get_exif_time(ExifApp* app)
{
  QDateTime res;

  ExifTag* tag = exif_find_tag(app, EXIF_IFD, 0x9003);      /* DateTimeOriginal from EXIF */
  if (! tag) {
    tag = exif_find_tag(app, IFD0, 0x0132);  /* DateTime from IFD0 */
  }
  if (! tag) {
    tag = exif_find_tag(app, EXIF_IFD, 0x9004);  /* DateTimeDigitized from EXIF */
  }

  if (tag) {
    char* str = exif_read_str(tag);
    res = QDateTime::fromString(str, "yyyy:MM:dd hh:mm:ss");
    xfree(str);
  }
  return res.toTime_t();
}

static Waypoint*
exif_waypt_from_exif_app(ExifApp* app)
{
  ExifTag* tag;
  char lat_ref = '\0';
  char lon_ref = '\0';
  char alt_ref = 0;
  char speed_ref = 'K';
  char* datum = nullptr;
  char mode = '\0';
  double gpsdop = unknown_alt;
  double alt = unknown_alt;
  time_t timestamp = UNKNOWN_TIMESTAMP;
  time_t datestamp = UNKNOWN_TIMESTAMP;

  ExifIfd* ifd = exif_find_ifd(app, GPS_IFD);
  if (ifd == nullptr) {
    return nullptr;
  }

  Waypoint* wpt = new Waypoint;

  wpt->latitude = unknown_alt;
  wpt->longitude = unknown_alt;

  for (auto& tag_instance : ifd->tags) {
    tag = &tag_instance;

    switch (tag->id) {
    case GPS_IFD_TAG_VERSION:
      break;
    case GPS_IFD_TAG_LATREF:
      lat_ref = tag->data.at(0);
      break;
    case GPS_IFD_TAG_LAT:
      wpt->latitude = exif_read_coord(tag);
      break;
    case GPS_IFD_TAG_LONREF:
      lon_ref = tag->data.at(0);
      break;
    case GPS_IFD_TAG_LON:
      wpt->longitude = exif_read_coord(tag);
      break;
    case GPS_IFD_TAG_ALTREF:
      alt_ref = tag->data.at(0);
      break;
    case GPS_IFD_TAG_ALT:
      alt = exif_read_double(tag, 0);
      break;
    case GPS_IFD_TAG_TIMESTAMP:
      timestamp = exif_read_timestamp(tag);
      break;
    case GPS_IFD_TAG_SAT:
      wpt->sat = atoi(tag->data.constData());
      break;
    case GPS_IFD_TAG_MODE:
      mode = tag->data.at(0);
      break;
    case GPS_IFD_TAG_DOP:
      gpsdop = exif_read_double(tag, 0);
      break;
    case GPS_IFD_TAG_SPEEDREF:
      speed_ref = tag->data.at(0);
      break;
    case GPS_IFD_TAG_SPEED:
      WAYPT_SET(wpt, speed, exif_read_double(tag, 0));
      break;
    case GPS_IFD_TAG_DATUM:
      datum = exif_read_str(tag);
      break;
    case GPS_IFD_TAG_DATESTAMP:
      datestamp = exif_read_datestamp(tag);
      break;
    }
  }

  if ((wpt->latitude == unknown_alt) || (wpt->longitude == unknown_alt)) {
    fatal(MYNAME ": Missing GPSLatitude and/or GPSLongitude!\n");
  }

  if (lat_ref == 'S') {
    wpt->latitude *= -1;
  } else if (lat_ref != 'N') {
    warning(MYNAME ": GPSLatitudeRef not set! Using N(orth).\n");
  }

  if (lon_ref == 'W') {
    wpt->longitude *= -1;
  } else if (lon_ref != 'E') {
    warning(MYNAME ": GPSLongitudeRef not set! Using E(east).\n");
  }

#ifdef EXIF_DBG
  printf(MYNAME "-GPSLatitude =  %12.7f\n", wpt->latitude);
  printf(MYNAME "-GPSLongitude = %12.7f\n", wpt->longitude);
#endif
  if (datum) {
    int idatum = gt_lookup_datum_index(datum, MYNAME);
    if (idatum < 0) {
      fatal(MYNAME ": Unknown GPSMapDatum \"%s\"!\n", datum);
    }
    if (idatum != DATUM_WGS84) {
      double alt;
      GPS_Math_WGS84_To_Known_Datum_M(wpt->latitude, wpt->longitude, 0.0,
                                      &wpt->latitude, &wpt->longitude, &alt, idatum);
    }
    xfree(datum);
  }

  if (alt != unknown_alt) {
    double sign;
    switch (alt_ref) {
    case 0:
      sign = 1.0;
      break;

    case 1:
      sign = -1.0;
      break;

    default:
      warning(MYNAME ": Invalid GPSAltitudeRef (%d)! Using default value 0 (= Sea level).\n", alt_ref);
      sign = 1.0;
    }
    wpt->altitude = sign * alt;
#ifdef EXIF_DBG
    printf(MYNAME "-GPSAltitude =  %12.7f m\n", wpt->altitude);
#endif
  }

  if WAYPT_HAS(wpt, speed) {
    switch (speed_ref) {
    case 'K':
      wpt->speed = KPH_TO_MPS(wpt->speed);
      break;
    case 'M':
      wpt->speed = MPH_TO_MPS(wpt->speed);
      break;
    case 'N':
      wpt->speed = KNOTS_TO_MPS(wpt->speed);
      break;
    default:
      wpt->speed = 0;
      WAYPT_UNSET(wpt, speed);
      warning(MYNAME ": Unknown GPSSpeedRef unit %c (0x%02x)!\n", speed_ref, speed_ref);
    }
#ifdef EXIF_DBG
    if WAYPT_HAS(wpt, speed) {
      printf(MYNAME "-GPSSpeed = %12.2f m/s\n", wpt->speed);
    }
#endif
  }

  if (mode == '2') {
    wpt->fix = fix_2d;
    if (gpsdop != unknown_alt) {
      wpt->hdop = gpsdop;
    }
  } else if (mode == '3') {
    wpt->fix = fix_3d;
    if (gpsdop != unknown_alt) {
      wpt->pdop = gpsdop;
    }
  }

  if (timestamp != UNKNOWN_TIMESTAMP) {
    if (datestamp != UNKNOWN_TIMESTAMP) {
      timestamp += datestamp;
    }
  } else {
    timestamp = datestamp;
  }

  if (timestamp != UNKNOWN_TIMESTAMP) {
#ifdef EXIF_DBG
    QString str =  QDateTime::fromTime_t(timestamp).toUTC().toString(Qt::ISODate);
    printf(MYNAME "-GPSTimeStamp =   %s\n", qPrintable(str));
#endif
    wpt->SetCreationTime(timestamp);
  } else {
    wpt->SetCreationTime(exif_get_exif_time(app));
  }

  tag = exif_find_tag(app, EXIF_IFD, EXIF_IFD_TAG_USER_CMT); /* UserComment */
  if (tag && (tag->size > 8)) {
    // TODO: User comments with JIS and Undefined Code Designations are ignored.
    if (memcmp(tag->data.constData(), "ASCII\0\0\0", 8) == 0) {
      wpt->notes = QString::fromLatin1(tag->data.constData() + 8, tag->size - 8);
    } else if (memcmp(tag->data.constData(), "UNICODE\0", 8) == 0) {
      QTextCodec* utf16_codec;
      if (app->fcache->big_endian) {
        utf16_codec = QTextCodec::codecForName("UTF-16BE");
      } else {
        utf16_codec = QTextCodec::codecForName("UTF-16LE");
      }
      wpt->notes = utf16_codec->toUnicode(tag->data.constData() + 8, tag->size - 8);
    }
  }

  if (opt_filename) {
    char* c;
    char* str = xstrdup(fin->name);

    char* cx = str;
    if ((c = strrchr(cx, ':'))) {
      cx = c + 1;
    }
    if ((c = strrchr(cx, '\\'))) {
      cx = c + 1;
    }
    if ((c = strrchr(cx, '/'))) {
      cx = c + 1;
    }
    if (((c = strchr(cx, '.'))) && (c != cx)) {
      *c = '\0';
    }
    wpt->shortname = cx;
    xfree(str);
  }

  return wpt;
}

static int exif_gcd(int ui, int vi)
{
  int u = abs(ui);
  int v = abs(vi);

  /* Modern Euclidean algorithum to find greatest commond divisor */
  /* See Knuth, Seminumerical Algorithms, pg. 320 */
  while (v != 0) {
    int r = u % v;
    u = v;
    v = r;
  }
  return u;
}

// TODO: This algorithm could be improved upon using continued fractions,
// i.e. the domain could be expanded and the accuracy improved.
// We could also achieve an increased domain and accuracy for RATIONAL
// types if we handled them separately.
static void
exif_dec2frac(double val, int32_t* num, int32_t* den)
{
  char sval[16], snum[16];
  char dot = 0;
  int den1 = 1;

  assert(val >= 0.0);
  if (val < 0.000000001) {
    val = 0.0;
  } else if (val > 999999999.0) {
    fatal(MYNAME ": Value (%f) to big for a rational representation!\n", val);
  }

  int num1 = 0;
  double vx = fabs(val);
  while (vx > 1) {
    num1++;
    vx = vx / 10;
  }

  snprintf(sval, sizeof(sval), "%*.*f", 9, 9 - num1, fabs(val));
  snum[0] = '\0';

  char* cx = sval;
  while (*cx) {
    if (dot) {
      den1 *= 10;
    }
    if (*cx == '.') {
      dot = 1;
    } else {
      strncat(snum, cx, 1);
    }
    cx++;
  }

  num1 = atoi(snum);

  int gcd = exif_gcd(num1, den1);
  assert(gcd != 0); // Note gcd(0, 0) = 0, but we shouldn't generate num1 = den1 = 0.

  *num = num1 / gcd;
  *den = den1 / gcd;
}

static ExifTag*
exif_put_value(const int ifd_nr, const uint16_t tag_id, const uint16_t type, const int count, const int index, const void* data)
{
  ExifTag* tag = nullptr;
  uint16_t size;

  ExifIfd* ifd = exif_find_ifd(exif_app, ifd_nr);
  if (ifd == nullptr) {
    exif_app->ifds.append(ExifIfd());
    ifd = &exif_app->ifds.last();
    ifd->nr = ifd_nr;
  } else {
    tag = exif_find_tag(exif_app, ifd_nr, tag_id);
  }

  uint16_t item_size = exif_type_size(type);

  if ((data == nullptr) || (count < 1) || (index < 0)) {
    size = 0;
  } else {
    size = (index + count) * item_size;
  }

  if (tag == nullptr) { /* create new tag */
    if (size == 0) {
      return nullptr;
    }

    ifd->tags.append(ExifTag());
    tag = &ifd->tags.last();

    tag->id = tag_id;
    tag->type = type;
    tag->count = index + count;
    tag->size = size;
    tag->data = QByteArray((size < 4) ? 4 : size, 0);
    ifd->count++;

  } else if (size == 0) { /* remove this element */
    ifd->count--;
    ifd->tags.removeOne(*tag);
    return nullptr;
  } else if (size > tag->data.size()) { /* modify existing tag */
    tag->data.append(size - tag->data.size(), 0);
    tag->size = size;
    tag->count = index + count;
  }

  switch (type) {
  case EXIF_TYPE_RAT:
  case EXIF_TYPE_SRAT: {
    double val = *(double*)data;
    uint32_t* dest = reinterpret_cast<uint32_t*>(tag->data.data());

    if ((val < 0.0) && (type == EXIF_TYPE_RAT)) {
      fatal(MYNAME ": A negative value cannot be stored as type RATIONAL.");
    }
    if ((int)val == val) {
      // For integers this expands the domain compared to the sub-optimal exif_dec2frac implementation.
      dest[index * 2] = (int)val;
      dest[(index * 2) + 1] = 1;
    } else {
      int32_t Nom, Den;
      exif_dec2frac(fabs(val), &Nom, &Den);
      if (val < 0.0) {
        Nom *= -1;
      }
      dest[index * 2] = Nom;
      dest[(index * 2) + 1] = Den;
    }
  }
  break;
  default: {
    char* dest = tag->data.data();
    memcpy(&dest[index * item_size], data, count * item_size);
  }
  }
  return tag;
}


static void
exif_put_double(const int ifd_nr, const int tag_id, const int index, const double val)
{
  // TODO: It seems wrong to toss the sign.
  double d = fabs(val);
  exif_put_value(ifd_nr, tag_id, EXIF_TYPE_RAT, 1, index, &d);
}


static void
exif_put_str(const int ifd_nr, const int tag_id, const char* val)
{
  int len = (val) ? strlen(val) + 1 : 0;
  exif_put_value(ifd_nr, tag_id, EXIF_TYPE_ASCII, len, 0, val);
}

static void
exif_put_coord(const int ifd_nr, const int tag_id, const double val)
{
  double vdeg;
  double vmin;
  double fractional_part;

  fractional_part = modf(val, &vdeg); 
  fractional_part = modf(60.0 * fractional_part, &vmin);
  double vsec = 60.0 * fractional_part;

  exif_put_double(ifd_nr, tag_id, 0, vdeg);
  exif_put_double(ifd_nr, tag_id, 1, vmin);
  exif_put_double(ifd_nr, tag_id, 2, vsec);
}

static void
exif_put_long(const int ifd_nr, const int tag_id, const int index, const int32_t val)
{
  exif_put_value(ifd_nr, tag_id, EXIF_TYPE_LONG, 1, index, &val);
}

static void
exif_remove_tag(const int ifd_nr, const int tag_id)
{
  exif_put_value(ifd_nr, tag_id, EXIF_TYPE_BYTE, 0, 0, nullptr);
}

static void
exif_find_wpt_by_time(const Waypoint* wpt)
{
  if (!wpt->creation_time.isValid()) {
    return;
  }

  if (exif_wpt_ref == nullptr) {
    exif_wpt_ref = wpt;
  } else if (labs(exif_time_ref - wpt->creation_time.toTime_t()) < labs(exif_time_ref - exif_wpt_ref->creation_time.toTime_t())) {
    exif_wpt_ref = wpt;
  }
}

static void
exif_find_wpt_by_name(const Waypoint* wpt)
{
  if (exif_wpt_ref != nullptr) {
    return;
  } else if ((wpt->shortname != nullptr) && (case_ignore_strcmp(wpt->shortname, opt_name) == 0)) {
    exif_wpt_ref = wpt;
  }
}


static bool
exif_sort_tags_cb(const ExifTag& A, const ExifTag& B)
{
  return A.id < B.id;
}

static bool
exif_sort_ifds_cb(const ExifIfd& A, const ExifIfd& B)
{
  return A.nr < B.nr;
}

static void
exif_write_value(ExifTag* tag, gbfile* fout)
{
  if (tag->size > 4) {
    gbfputuint32(tag->offset, fout);  /* offset to data */
  } else {
    char* data = tag->data.data();

    if BYTE_TYPE(tag->type) {
      gbfwrite(data, 4, 1, fout);
    } else if WORD_TYPE(tag->type) {
      gbfputuint16(*(uint16_t*)data, fout);
      gbfputuint16(*(uint16_t*)(data+2), fout);
    } else if LONG_TYPE(tag->type) {
      gbfputuint32(*(uint32_t*)data, fout);
    } else if (tag->type == EXIF_TYPE_FLOAT) {
      gbfputflt(*(float*)data, fout);
    } else {
      fatal(MYNAME ": Unknown data type %d!\n", tag->type);
    }
  }
}

static void
exif_write_ifd(ExifIfd* ifd, const char next, gbfile* fout)
{
  gbfputuint16(ifd->count, fout);
  gbsize_t offs = gbftell(fout) + (ifd->count * 12) + 4;

  for (auto& tag_instance : ifd->tags) {
    ExifTag* tag = &tag_instance;

    gbfputuint16(tag->id, fout);
    gbfputuint16(tag->type, fout);
    gbfputuint32(tag->count, fout);
    if (tag->size > 4) {
      tag->offset = offs;
      offs += tag->size;
      if (offs & 1u) {
        offs++;
      }
      gbfputuint32(tag->offset, fout);
    } else {
      exif_write_value(tag, fout);
    }
  }

  if (next) {
    gbfputuint32(offs, fout);
  } else {
    gbfputuint32(0, fout);
  }

  for (auto& tag_instance : ifd->tags) {
    ExifTag* tag = &tag_instance;

    if (tag->size > 4) {
      char* ptr = tag->data.data();

      if BYTE_TYPE(tag->type) {
        gbfwrite(tag->data.data(), tag->size, 1, fout);
      } else for (uint16_t i = 0; i < tag->count; i++) {
          switch (tag->type) {
          case EXIF_TYPE_SHORT:
          case EXIF_TYPE_SSHORT:
            gbfputuint16(*(int16_t*)ptr, fout);
            break;
          case EXIF_TYPE_LONG:
          case EXIF_TYPE_SLONG:
          case EXIF_TYPE_IFD:
            gbfputuint32(*(int32_t*)ptr, fout);
            break;
          case EXIF_TYPE_RAT:
          case EXIF_TYPE_SRAT:
            gbfputuint32(*(int32_t*)ptr, fout);
            gbfputuint32(*(int32_t*)(ptr+4), fout);
            break;
          case EXIF_TYPE_FLOAT:
            gbfputflt(*(float*)ptr, fout);
            break;
          case EXIF_TYPE_DOUBLE:
            gbfputdbl(*(double*)ptr, fout);
            break;
          default:
            gbfwrite(ptr, exif_type_size(tag->type), 1, fin);
            break;
          }
          ptr += (tag->size / tag->count);
        }
      if (gbftell(fout) & 1u) {
        gbfputc(0, fout);
      }
    }
  }
}

static void
exif_write_apps()
{
  gbfputuint16(0xFFD8, fout);

  for (auto& app_instance : *exif_apps) {
    ExifApp* app = &app_instance;

    gbfputuint16(app->marker, fout);

    if (app == exif_app) {
      assert(app->marker == 0xFFE1);
      uint16_t len = 8;
      ExifTag* tag;

      exif_put_long(IFD0, IFD0_TAG_GPS_IFD_OFFS, 0, 0);
      exif_put_value(GPS_IFD, GPS_IFD_TAG_VERSION, EXIF_TYPE_BYTE, 4, 0, writer_gps_tag_version);

      std::sort(app->ifds.begin(), app->ifds.end(), exif_sort_ifds_cb);

      for (auto& ifd_instance : app->ifds) {
        ExifIfd* ifd = &ifd_instance;

        if (ifd->nr == GPS_IFD) {
          exif_put_long(IFD0, IFD0_TAG_GPS_IFD_OFFS, 0, len);
        } else if (ifd->nr == EXIF_IFD) {
          exif_put_long(IFD0, IFD0_TAG_EXIF_IFD_OFFS, 0, len);
        } else if (ifd->nr == INTER_IFD) {
          exif_put_long(EXIF_IFD, EXIF_IFD_TAG_INTER_IFD_OFFS, 0, len);
        }

        len += exif_ifd_size(ifd);
      }

      len += 4; /* DWORD(0) after last ifd */

      if ((exif_find_tag(app, IFD1, IFD1_TAG_JPEG_OFFS))) {
        exif_put_long(IFD1, IFD1_TAG_JPEG_OFFS, 0, len);
      }

      for (auto& ifd_instance : app->ifds) {
        ExifIfd* ifd = &ifd_instance;
        std::sort(ifd->tags.begin(), ifd->tags.end(), exif_sort_tags_cb);
      }

      gbfile* ftmp = gbfopen_be(nullptr, "wb", MYNAME);
      ftmp->big_endian = app->fcache->big_endian;

      gbfwrite((ftmp->big_endian) ? "MM" : "II", 2, 1, ftmp);
      gbfputuint16(0x2A, ftmp);
      gbfputuint32(0x08, ftmp); /* offset to first IFD */

      for (int i = 0; i < app->ifds.size(); ++i) {
        ExifIfd* ifd = &app->ifds[i];

        char next = ((ifd->nr == IFD0) && ((i + 1) < app->ifds.size()) && (app->ifds[i+1].nr == IFD1));

        exif_write_ifd(ifd, next, ftmp);
        len = gbftell(ftmp);
      }

      gbfputuint32(0, ftmp); /* DWORD(0) after last ifd */

      if ((tag = exif_find_tag(app, IFD1, IFD1_TAG_JPEG_OFFS))) {
        gbsize_t offs = tag->original;
        if ((tag = exif_find_tag(app, IFD1, IFD1_TAG_JPEG_SIZE))) {
          gbfseek(app->fexif, offs, SEEK_SET);
          gbfcopyfrom(ftmp, app->fexif, tag->toLong());
        }
      }

      len = gbftell(ftmp);
      gbfrewind(ftmp);
      gbfputuint16(len + 8, fout);
      gbfwrite("Exif\0\0", 6, 1, fout);
      gbfcopyfrom(fout, ftmp, len);

      gbfclose(ftmp);
    } else {
      gbfputuint16(app->len, fout);
      gbfrewind(app->fcache);
      gbfcopyfrom(fout, app->fcache, 0x7FFFFFFF);
    }
  }
}

/*******************************************************************************
* %%%        global callbacks called by gpsbabel main process              %%% *
*******************************************************************************/

static void
exif_rd_init(const QString& fname)
{
  fin = gbfopen_be(fname, "rb", MYNAME);
  exif_apps = new QList<ExifApp>;
}

static void
exif_rd_deinit()
{
  exif_release_apps();
  gbfclose(fin);
}

static void
exif_read()
{
  uint16_t soi = gbfgetuint16(fin);
  is_fatal(soi != 0xFFD8, MYNAME ": Unknown image file.");  /* only jpeg for now */

  exif_app = exif_load_apps();
  is_fatal(exif_app == nullptr, MYNAME ": No EXIF header in source file \"%s\".", fin->name);

  exif_examine_app(exif_app);
  Waypoint* wpt = exif_waypt_from_exif_app(exif_app);
  if (wpt) {
    waypt_add(wpt);
  }
}

static void
exif_wr_init(const QString& fname)
{
  exif_success = 0;
  exif_fout_name = fname;

  exif_apps = new QList<ExifApp>;

  fin = gbfopen_be(fname, "rb", MYNAME);
  is_fatal(fin->is_pipe, MYNAME ": Sorry, this format cannot be used with pipes!");

  uint16_t soi = gbfgetuint16(fin);
  is_fatal(soi != 0xFFD8, MYNAME ": Unknown image file.");
  exif_app = exif_load_apps();
  is_fatal(exif_app == nullptr, MYNAME ": No EXIF header found in source file \"%s\".", fin->name);
  exif_examine_app(exif_app);
  gbfclose(fin);

  exif_time_ref = exif_get_exif_time(exif_app);
  if (exif_time_ref == 0) {
    fatal(MYNAME ": No valid timestamp found in picture!\n");
  }

  QString filename(fname);
  filename += ".jpg";
  fout = gbfopen_be(filename, "wb", MYNAME);
}

static void
exif_wr_deinit()
{

  exif_release_apps();
  QString tmpname = QString(fout->name);
  gbfclose(fout);

  if (exif_success) {
    if (*opt_overwrite == '1') {
      QFile::remove(exif_fout_name);
      QFile::rename(tmpname, exif_fout_name);
    }
  } else {
    QFile::remove(tmpname);
  }

  exif_fout_name.clear();
}

static void
exif_write()
{
  exif_wpt_ref = nullptr;

  if (opt_name) {
    waypt_disp_all(exif_find_wpt_by_name);
    if (exif_wpt_ref == nullptr) {
      route_disp_all(nullptr, nullptr, exif_find_wpt_by_name);
    }
    if (exif_wpt_ref == nullptr) {
      track_disp_all(nullptr, nullptr, exif_find_wpt_by_name);
    }
    if (exif_wpt_ref == nullptr) {
      warning(MYNAME ": No matching point with name \"%s\" found.\n", opt_name);
    }
  } else {
    QString str = exif_time_str(exif_time_ref);

    track_disp_all(nullptr, nullptr, exif_find_wpt_by_time);
    route_disp_all(nullptr, nullptr, exif_find_wpt_by_time);
    waypt_disp_all(exif_find_wpt_by_time);

    time_t frame = atoi(opt_frame);

    if (exif_wpt_ref == nullptr) {
      warning(MYNAME ": No point with a valid timestamp found.\n");
    } else if (labs(exif_time_ref - exif_wpt_ref->creation_time.toTime_t()) > frame) {
      warning(MYNAME ": No matching point found for image date %s!\n", qPrintable(str));
      if (exif_wpt_ref != nullptr) {
        QString str = exif_time_str(exif_wpt_ref->creation_time.toTime_t());
        warning(MYNAME ": Best is from %s, %ld second(s) away.\n",
                qPrintable(str), labs(exif_time_ref - exif_wpt_ref->creation_time.toTime_t()));
      }
      exif_wpt_ref = nullptr;
    }
  }

  if (exif_wpt_ref != nullptr) {
    const Waypoint* wpt = exif_wpt_ref;

    exif_put_long(IFD0, IFD0_TAG_GPS_IFD_OFFS, 0, 0);
    exif_put_value(GPS_IFD, GPS_IFD_TAG_VERSION, EXIF_TYPE_BYTE, 4, 0, writer_gps_tag_version);
    exif_put_str(GPS_IFD, GPS_IFD_TAG_DATUM, "WGS-84");

    exif_put_str(GPS_IFD, GPS_IFD_TAG_LATREF, wpt->latitude < 0 ? "S" : "N");
    exif_put_coord(GPS_IFD, GPS_IFD_TAG_LAT, fabs(wpt->latitude));
    exif_put_str(GPS_IFD, GPS_IFD_TAG_LONREF, wpt->longitude < 0 ? "W" : "E");
    exif_put_coord(GPS_IFD, GPS_IFD_TAG_LON, fabs(wpt->longitude));

    if (wpt->altitude == unknown_alt) {
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_ALT);
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_ALTREF);
    } else {
      uint8_t alt_ref;
      if (wpt->altitude >= 0.0) {
        alt_ref = 0;
      } else {
        alt_ref = 1;
      }
      exif_put_value(GPS_IFD, GPS_IFD_TAG_ALTREF, EXIF_TYPE_BYTE, 1, 0, &alt_ref);
      exif_put_double(GPS_IFD, GPS_IFD_TAG_ALT, 0, fabs(wpt->altitude));
    }

    if (wpt->creation_time.isValid()) {
      char buf[32];

      const time_t tt = wpt->GetCreationTime().toTime_t();
      struct tm tm = *gmtime(&tt);

      tm.tm_year += 1900;
      tm.tm_mon += 1;

      exif_put_double(GPS_IFD, GPS_IFD_TAG_TIMESTAMP, 0, tm.tm_hour);
      exif_put_double(GPS_IFD, GPS_IFD_TAG_TIMESTAMP, 1, tm.tm_min);
      exif_put_double(GPS_IFD, GPS_IFD_TAG_TIMESTAMP, 2, tm.tm_sec);

      snprintf(buf, sizeof(buf), "%04d:%02d:%02d", tm.tm_year, tm.tm_mon, tm.tm_mday);
      exif_put_str(GPS_IFD, GPS_IFD_TAG_DATESTAMP, buf);
    } else {
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_TIMESTAMP);
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_DATESTAMP);
    }

    if (wpt->sat > 0) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", wpt->sat);
      exif_put_str(GPS_IFD, GPS_IFD_TAG_SAT, buf);
    } else {
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_SAT);
    }

    if (wpt->fix == fix_2d) {
      exif_put_str(GPS_IFD, GPS_IFD_TAG_MODE, "2");
    } else if (wpt->fix == fix_3d) {
      exif_put_str(GPS_IFD, GPS_IFD_TAG_MODE, "3");
    } else {
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_MODE);
    }

    if (wpt->hdop > 0) {
      exif_put_double(GPS_IFD, GPS_IFD_TAG_DOP, 0, wpt->hdop);
    } else {
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_DOP);
    }

    if WAYPT_HAS(wpt, speed) {
      exif_put_str(GPS_IFD, GPS_IFD_TAG_SPEEDREF, "K");
      exif_put_double(GPS_IFD, GPS_IFD_TAG_SPEED, 0, MPS_TO_KPH(wpt->speed));
    } else {
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_SPEEDREF);
      exif_remove_tag(GPS_IFD, GPS_IFD_TAG_SPEED);
    }

    exif_write_apps();  /* Success, write the new file */

    exif_success = 1;
  }

}

/**************************************************************************/

ff_vecs_t exif_vecs = {
  ff_type_file,
  {
    (ff_cap)(ff_cap_read | ff_cap_write)  /* waypoints */,
    ff_cap_none       /* tracks */,
    ff_cap_none       /* routes */
  },
  exif_rd_init,
  exif_wr_init,
  exif_rd_deinit,
  exif_wr_deinit,
  exif_read,
  exif_write,
  nullptr,
  exif_args,
  CET_CHARSET_UTF8, 0,
  NULL_POS_OPS,
  nullptr
};

/**************************************************************************/
