/* packer.cpp --

   This file is part of the UPX executable compressor.

   Copyright (C) 1996-2001 Markus Franz Xaver Johannes Oberhumer
   Copyright (C) 1996-2001 Laszlo Molnar
   All Rights Reserved.

   UPX and the UCL library are free software; you can redistribute them
   and/or modify them under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.
   If not, write to the Free Software Foundation, Inc.,
   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

   Markus F.X.J. Oberhumer   Laszlo Molnar
   markus@oberhumer.com      ml1050@cdata.tvnet.hu
 */


//#define WANT_STL
#include "conf.h"
#include "version.h"
#include "file.h"
#include "packer.h"
#include "filter.h"
#include "linker.h"
#include "ui.h"


/*************************************************************************
//
**************************************************************************/

Packer::Packer(InputFile *f) :
    fi(f), file_size(-1), ph_format(-1), ph_version(-1),
    uip(NULL), ui_pass(0), ui_total_passes(0), linker(NULL),
    last_patch(NULL), last_patch_len(0), last_patch_off(0)
{
    file_size = f->st.st_size;
    uip = new UiPacker(this);
    memset(&ph, 0, sizeof(ph));
}


Packer::~Packer()
{
    delete uip; uip = NULL;
    delete linker; linker = NULL;
}


/*************************************************************************
// public entries called from class PackMaster
**************************************************************************/

void Packer::doPack(OutputFile *fo)
{
    uip->uiPackStart(fo);
    pack(fo);
    uip->uiPackEnd(fo);
}

void Packer::doUnpack(OutputFile *fo)
{
    uip->uiUnpackStart(fo);
    unpack(fo);
    uip->uiUnpackEnd(fo);
}

void Packer::doTest()
{
    uip->uiTestStart();
    test();
    uip->uiTestEnd();
}

void Packer::doList()
{
    uip->uiListStart();
    list();
    uip->uiListEnd();
}

void Packer::doFileInfo()
{
    uip->uiFileInfoStart();
    fileInfo();
    uip->uiFileInfoEnd();
}


/*************************************************************************
// default actions
**************************************************************************/

void Packer::test()
{
    unpack(NULL);
}


void Packer::list()
{
    uip->uiList();
}


void Packer::fileInfo()
{
    // FIXME: subclasses should list their sections here
    // We also should try to get a nice layout...
}


bool Packer::testUnpackVersion(int version) const
{
    if (version != ph_version && ph_version != -1)
        throwCantUnpack("program has been modified; run a virus checker!");
    if (!canUnpackVersion(version))
        throwCantUnpack("I am not compatible with older versions of UPX");
    return true;
}


bool Packer::testUnpackFormat(int format) const
{
    if (format != ph_format && ph_format != -1)
        throwCantUnpack("program has been modified; run a virus checker!");
    return canUnpackFormat(format);
}


/*************************************************************************
// compress
**************************************************************************/

bool Packer::compress(upx_bytep in, upx_bytep out,
                      unsigned max_offset, unsigned max_match)
{
#if defined(UNUPX)
    throwInternalError("compression failed");
    return false;
#else
    ph.c_len = 0;
    assert(ph.level >= 1); assert(ph.level <= 10);

    // save current checksums
    ph.saved_u_adler = ph.u_adler;
    ph.saved_c_adler = ph.c_adler;
    // update checksum of uncompressed data
    ph.u_adler = upx_adler32(ph.u_adler,in,ph.u_len);

    // set compression paramters
    upx_compress_config_t conf;
    memset(&conf, 0xff, sizeof(conf));
    // arguments
    if (max_offset != 0)
        conf.max_offset = max_offset;
    if (max_match != 0)
        conf.max_match = max_match;
    // options
    if (opt->crp.c_flags != -1)
        conf.c_flags = opt->crp.c_flags;
#if 0
    else
        // this is based on experimentation
        conf.c_flags = (ph.level >= 7) ? 0 : 1 | 2;
#endif
    if (opt->crp.p_level != -1)
        conf.p_level = opt->crp.p_level;
    if (opt->crp.h_level != -1)
        conf.h_level = opt->crp.h_level;
    if (opt->crp.max_offset != UPX_UINT_MAX && opt->crp.max_offset < conf.max_offset)
        conf.max_offset = opt->crp.max_offset;
    if (opt->crp.max_match != UPX_UINT_MAX && opt->crp.max_match < conf.max_match)
        conf.max_match = opt->crp.max_match;

    // Avoid too many progress bar updates. 64 is s->bar_len in ui.cpp.
    unsigned step = (ph.u_len < 64*1024) ? 0 : ph.u_len / 64;
#if defined(WITH_NRV)
    if (ph.level >= 7 || (ph.level >= 4 && ph.u_len >= 512*1024))
        step = 0;
#endif
    if (ui_pass >= 0)
        ui_pass++;
    uip->startCallback(ph.u_len, step, ui_pass, ui_total_passes);
    uip->firstCallback();

    //OutputFile::dump("data.raw", in, ph.u_len);

    // compress
    unsigned result[16];
    memset(result, 0, sizeof(result));
    int r = upx_compress(in, ph.u_len, out, &ph.c_len,
                         uip->getCallback(),
                         ph.method, ph.level, &conf, result);

    //uip->finalCallback(ph.u_len, ph.c_len);
    uip->endCallback();

    if (r == UPX_E_OUT_OF_MEMORY)
        throwCantPack("out of memory");
    if (r != UPX_E_OK)
        throwInternalError("compression failed");

    //ph.min_offset_found = result[0];
    ph.max_offset_found = result[1];
    //ph.min_match_found = result[2];
    ph.max_match_found = result[3];
    //ph.min_run_found = result[4];
    ph.max_run_found = result[5];
    ph.first_offset_found = result[6];
    //ph.same_match_offsets_found = result[7];
    assert(max_offset == 0 || max_offset >= ph.max_offset_found);
    assert(max_match == 0 || max_match >= ph.max_match_found);

    //printf("\nPacker::compress: %d/%d: %7d -> %7d\n", ph.method, ph.level, ph.u_len, ph.c_len);
    if (!checkCompressionRatio(ph.u_len, ph.c_len))
        return false;
    // return in any case if not compressible
    if (ph.c_len >= ph.u_len)
        return false;

    // update checksum of compressed data
    ph.c_adler = upx_adler32(ph.c_adler,out,ph.c_len);
    // Decompress and verify. Skip this when using the fastest level.
    if (ph.level > 1)
    {
        // decompress
        unsigned new_len = ph.u_len;
        r = upx_decompress(out,ph.c_len,in,&new_len,ph.method);
        //printf("%d %d: %d %d %d\n", ph.method, r, ph.c_len, ph.u_len, new_len);
        if (r != UPX_E_OK)
            throwInternalError("decompression failed");
        if (new_len != ph.u_len)
            throwInternalError("decompression failed (size error)");

        // verify decompression
        if (ph.u_adler != upx_adler32(ph.saved_u_adler,in,ph.u_len))
            throwInternalError("decompression failed (checksum error)");
    }
    return true;
#endif /* UNUPX */
}


bool Packer::checkCompressionRatio(unsigned u_len, unsigned c_len) const
{
    assert((int)u_len > 0);
    assert((int)c_len > 0);

#if 1
    if (c_len + 512 >= u_len)           // min. 512 bytes gain
        return false;
    if (c_len >= u_len - u_len / 8)     // min. 12.5% gain
        return false;
#else
    if (c_len >= u_len)
        return false;
#endif
    return true;
}


bool Packer::checkFinalCompressionRatio(const OutputFile *fo) const
{
    const unsigned u_len = file_size;
    const unsigned c_len = fo->getBytesWritten();

    assert((int)u_len > 0);
    assert((int)c_len > 0);

    if (c_len + 4096 <= u_len)          // ok if we have 4096 bytes gain
        return true;

    if (c_len + 512 >= u_len)           // min. 512 bytes gain
        return false;
    if (c_len >= u_len - u_len / 8)     // min. 12.5% gain
        return false;

    return true;
}


/*************************************************************************
// decompress
**************************************************************************/

void Packer::decompress(const upx_bytep in, upx_bytep out,
                        bool verify_checksum)
{
    unsigned adler;

    // verify checksum of compressed data
    if (verify_checksum)
    {
        adler = upx_adler32(ph.saved_c_adler, in, ph.c_len);
        if (adler != ph.c_adler)
            throwChecksumError();
    }

    // decompress
    unsigned new_len = ph.u_len;
    int r = upx_decompress(in, ph.c_len, out, &new_len, ph.method);
    if (r != UPX_E_OK || new_len != ph.u_len)
        throwCompressedDataViolation();

    // verify checksum of decompressed data
    if (verify_checksum)
    {
        adler = upx_adler32(ph.saved_u_adler, out, ph.u_len);
        if (adler != ph.u_adler)
            throwChecksumError();
    }
}


/*************************************************************************
// overlapping decompression
**************************************************************************/

bool Packer::testOverlappingDecompression(const upx_bytep buf,
                                          unsigned overlap_overhead) const
{
#if defined(UNUPX)
    UNUSED(buf);
    UNUSED(overlap_overhead);
    return false;
#else
    if (ph.c_len >= ph.u_len)
        return false;

    assert((int)overlap_overhead >= 0);
    // Because we are not using the asm_fast decompressor here
    // we must account for extra 3 bytes or else we may fail
    // at runtime decompression.
    if (overlap_overhead <= 4 + 3)  // don't waste time here
        return false;
    overlap_overhead -= 3;

    unsigned src_off = ph.u_len + overlap_overhead - ph.c_len;
    unsigned new_len = ph.u_len;
    int r = upx_test_overlap(buf - src_off, src_off,
                             ph.c_len, &new_len, ph.method);
    return (r == UPX_E_OK && new_len == ph.u_len);
#endif /* UNUPX */
}


void Packer::verifyOverlappingDecompression()
{
    assert(ph.c_len < ph.u_len);
    assert((int)ph.overlap_overhead > 0);
#if 1 && !defined(UNUPX)
    // Idea:
    //   obuf[] was allocated with MemBuffer::allocForCompression(), and
    //   its contents are no longer needed, i.e. the compressed data
    //   must have been already written.
    //   We now can perform a real overlapping decompression and
    //   verify the checksum.
    //
    // Note:
    //   This verify is just because of complete paranoia that there
    //   could be a hidden bug in the upx_test_overlap implementation,
    //   and it should not be necessary at all.
    //
    // See also:
    //   Filter::verifyUnfilter()

    if (ph.level == 1)
        return;
    unsigned offset = (ph.u_len + ph.overlap_overhead) - ph.c_len;
    if (offset + ph.c_len > obuf.getSize())
        return;
    memmove(obuf + offset, obuf, ph.c_len);
    decompress(obuf + offset, obuf, true);
#endif /* !UNUPX */
}


/*************************************************************************
// Find overhead for in-place decompression in an heuristic way
// (using a binary search). Return 0 on error.
//
// To speed up things:
//   - you can pass the range of an acceptable interval (so that
//     we can succeed early)
//   - you can enforce an upper_limit (so that we can fail early)
**************************************************************************/

unsigned Packer::findOverlapOverhead(const upx_bytep buf,
                                     unsigned range,
                                     unsigned upper_limit) const
{
#if defined(UNUPX)
    throwInternalError("not implemented");
    return 0;
#else
    assert((int) range >= 0);

    // prepare to deal with very pessimistic values
    unsigned low = 1;
    unsigned high = UPX_MIN(ph.u_len / 4 + 512, upper_limit);
    // but be optimistic for first try (speedup)
    unsigned m = UPX_MIN(16, high);
    //
    unsigned overhead = 0;
    unsigned nr = 0;          // statistics

    while (high >= low)
    {
        assert(m >= low); assert(m <= high);
        assert(m < overhead || overhead == 0);
        nr++;
        if (testOverlappingDecompression(buf, m))
        {
            overhead = m;
            // Succeed early if m lies in [low .. low+range-1], i.e. if
            // if the range of the current interval is <= range.
            //if (m <= low + range - 1)
            if (m + 1 <= low + range)   // avoid underflow
                break;
            high = m - 1;
        }
        else
            low = m + 1;
        m = (low + high) / 2;
    }

    //printf("findOverlapOverhead: %d (%d tries)\n", overhead, nr);
    if (overhead == 0)
        throwInternalError("this is an oo bug");

    UNUSED(nr);
    return overhead;
#endif /* UNUPX */
}


/*************************************************************************
// file i/o utils
**************************************************************************/

void Packer::handleStub(InputFile *fif, OutputFile *fo, long size)
{
    if (fo)
    {
        if (size > 0)
        {
            // copy stub from exe
            info("Copying original stub: %ld bytes", size);
            autoheap_array(char, stub, size);
            fif->seek(0,SEEK_SET);
            fif->readx(stub,size);
            fo->write(stub,size);
        }
        else
        {
            // no stub
        }
    }
}


void Packer::checkOverlay(unsigned overlay)
{
    assert((off_t)overlay >= 0);
    assert((off_t)overlay < file_size);
    if (overlay == 0)
        return;
    info("Found overlay: %d bytes", overlay);
    if (opt->overlay == opt->SKIP_OVERLAY)
        throw OverlayException("file has overlay -- skipped; try `--overlay=copy'");
}


void Packer::copyOverlay(OutputFile *fo, unsigned overlay,
                         MemBuffer *buf,
                         bool do_seek)
{
    assert((int)overlay >= 0);
    assert((off_t)overlay < file_size);
    if (!fo || overlay == 0)
        return;
    if (opt->overlay != opt->COPY_OVERLAY)
    {
        assert(opt->overlay == opt->STRIP_OVERLAY);
        infoWarning("stripping overlay: %d bytes", overlay);
        return;
    }
    info("Copying overlay: %d bytes", overlay);
    if (do_seek)
        fi->seek(-(off_t)overlay, SEEK_END);

    // get buffer size, align to improve i/o speed
    unsigned buf_size = buf->getSize();
    if (buf_size > 65536)
        buf_size = ALIGN_DOWN(buf_size, 4096);
    assert((int)buf_size > 0);

    do {
        unsigned len = overlay < buf_size ? overlay : buf_size;
        fi->readx(*buf, len);
        fo->write(*buf, len);
        overlay -= len;
    } while (overlay > 0);
}


// Create a pseudo-unique program id.
unsigned Packer::getRandomId() const
{
    unsigned id = 0;
#if 0 && defined(__unix__)
    // Don't consume precious bytes from /dev/urandom.
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        fd = open("/dev/random", O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &id, 4) != 4)
            id = 0;
        close(fd);
    }
#endif
    while (id == 0)
    {
#if !defined(HAVE_GETTIMEOFDAY) || defined(__DJGPP__)
        id ^= (unsigned) time(NULL);
        id ^= ((unsigned) clock()) << 12;
#else
        struct timeval tv;
        gettimeofday(&tv, 0);
        id ^= (unsigned) tv.tv_sec;
        id ^= ((unsigned) tv.tv_usec) << 12;  // shift into high-bits
#endif
#if defined(HAVE_GETPID)
        id ^= (unsigned) getpid();
#endif
        id ^= (unsigned) fi->st.st_ino;
        id ^= (unsigned) fi->st.st_atime;
        id ^= (unsigned) rand();
    }
    return id;
}


/*************************************************************************
// packheader util
**************************************************************************/

// this is called directly after the constructor from class PackMaster
void Packer::initPackHeader()
{
    memset(&ph, 0, sizeof(ph));
    ph.version = getVersion();
    ph.format = getFormat();
    ph.method = -1;
    ph.level = -1;
    ph.u_adler = ph.c_adler = ph.saved_u_adler = ph.saved_c_adler = upx_adler32(0,NULL,0);
    ph.buf_offset = -1;
    ph.u_file_size = file_size;
}


// this is called directly after canPack() from class PackMaster
void Packer::updatePackHeader()
{
    assert(opt->cmd == CMD_COMPRESS);
    //
    const int *m = getCompressionMethods(opt->method, opt->level);
    ph.method = m[0];
    ph.level = opt->level;
    if (ph.level < 0)
        ph.level = file_size < 512*1024 ? 8 : 7;
    //
    assert(isValidCompressionMethod(ph.method));
    assert(1 <= ph.level && ph.level <= 10);
}


int Packer::patchPackHeader(void *b, int blen)
{
    const int size = ph.getPackHeaderSize();
    assert(isValidFilter(ph.filter));

    int boff = find_le32(b, blen, UPX_MAGIC_LE32);
    checkPatch(b, blen, boff, size);

    unsigned char *p = (unsigned char *)b + boff;
    ph.putPackHeader(p);

    return boff;
}


bool Packer::getPackHeader(void *b, int blen)
{
    if (!ph.fillPackHeader((unsigned char *)b, blen))
        return false;

    if (ph.version > getVersion())
        throwCantUnpack("need a newer version of UPX");
    // Some formats might be able to unpack old versions because
    // their implementation hasn't changed. Ask them.
    if (opt->cmd != CMD_FILEINFO)
        if (!testUnpackVersion(ph.version))
            return false;

    if (ph.c_len >= ph.u_len || (off_t)ph.c_len >= file_size
        || ph.version <= 0 || ph.version >= 0xff)
        throwCantUnpack("header corrupted");
    else if ((off_t)ph.u_len > ph.u_file_size)
    {
#if 0
        // FIXME: does this check make sense w.r.t. overlays ???
        if (ph.format == UPX_F_W32_PE || ph.format == UPX_F_DOS_EXE)
            // may get longer
            ((void)0);
        else
            throwCantUnpack("header size corrupted");
#endif
    }
    if (!isValidCompressionMethod(ph.method))
        throwCantUnpack("unknown compression method");

    // Some formats might be able to unpack "subformats". Ask them.
    if (!testUnpackFormat(ph.format))
        return false;

    return true;
}


bool Packer::readPackHeader(int len)
{
    assert((int)len > 0);
    MemBuffer buf(len);
    len = fi->read(buf, len);
    if (len <= 0)
        return false;
    return getPackHeader(buf, len);
}


void Packer::checkAlreadyPacked(void *b, int blen)
{
    int boff = find_le32(b, blen, UPX_MAGIC_LE32);
    if (boff < 0)
        return;

    // FIXME: could add some more checks to verify that this
    //   is a real PackHeader, e.g.
    //
    //PackHeader tmp;
    //if (!tmp.fillPackHeader((unsigned char *)b + boff, blen - boff))
    //    return;
    //
    // This also would require that the buffer in `b' holds
    // the full PackHeader, and not only the 4 magic bytes.

    throwAlreadyPacked();
}


bool Packer::isValidCompressionMethod(int method)
{
    return (method >= M_NRV2B_LE32 && method <= M_NRV2D_LE16);
}


/*************************************************************************
// patch util for loader
**************************************************************************/

void Packer::checkPatch(void *b, int blen, int boff, int size)
{
    if (b == NULL && blen == 0 && boff == 0 && size == 0)
    {
        // reset
        last_patch = NULL;
        last_patch_len = 0;
        last_patch_off = 0;
        return;
    }
    if (b == NULL || blen <= 0 || boff < 0 || size <= 0)
        throwBadLoader();
    if (boff + size <= 0 || boff + size > blen)
        throwBadLoader();
    //printf("checkPatch: %p %5d %5d %2d\n", b, blen, boff, size);
    if (b == last_patch)
    {
        if (boff + size > last_patch_off)
            throwInternalError("invalid patch order");
        // The next check is not strictly necessary, but the buffer
        // length should better not increase...
        if (blen > last_patch_len)
            throwInternalError("invalid patch order (length)");
    }
    else
        last_patch = b;
    last_patch_len = blen;
    last_patch_off = boff;
}


int Packer::patch_be16(void *b, int blen, unsigned old, unsigned new_)
{
    int boff = find_be16(b, blen, old);
    checkPatch(b, blen, boff, 2);

    unsigned char *p = (unsigned char *)b + boff;
    set_be16(p,new_);

    return boff;
}


int Packer::patch_be16(void *b, int blen, const void *old, unsigned new_)
{
    int boff = find(b, blen, old, 2);
    checkPatch(b, blen, boff, 2);

    unsigned char *p = (unsigned char *)b + boff;
    set_be16(p,new_);

    return boff;
}


int Packer::patch_be32(void *b, int blen, unsigned old, unsigned new_)
{
    int boff = find_be32(b, blen, old);
    checkPatch(b, blen, boff, 4);

    unsigned char *p = (unsigned char *)b + boff;
    set_be32(p,new_);

    return boff;
}


int Packer::patch_be32(void *b, int blen, const void *old, unsigned new_)
{
    int boff = find(b, blen, old, 4);
    checkPatch(b, blen, boff, 4);

    unsigned char *p = (unsigned char *)b + boff;
    set_be32(p,new_);

    return boff;
}


int Packer::patch_le16(void *b, int blen, unsigned old, unsigned new_)
{
    int boff = find_le16(b, blen, old);
    checkPatch(b, blen, boff, 2);

    unsigned char *p = (unsigned char *)b + boff;
    set_le16(p,new_);

    return boff;
}


int Packer::patch_le16(void *b, int blen, const void *old, unsigned new_)
{
    int boff = find(b, blen, old, 2);
    checkPatch(b, blen, boff, 2);

    unsigned char *p = (unsigned char *)b + boff;
    set_le16(p,new_);

    return boff;
}


int Packer::patch_le32(void *b, int blen, unsigned old, unsigned new_)
{
    int boff = find_le32(b, blen, old);
    checkPatch(b, blen, boff, 4);

    unsigned char *p = (unsigned char *)b + boff;
    set_le32(p,new_);

    return boff;
}


int Packer::patch_le32(void *b, int blen, const void *old, unsigned new_)
{
    int boff = find(b, blen, old, 4);
    checkPatch(b, blen, boff, 4);

    unsigned char *p = (unsigned char *)b + boff;
    set_le32(p,new_);

    return boff;
}


// patch version into stub/ident_n.ash
int Packer::patchVersion(void *b, int blen)
{
    int boff = find(b, blen, "$Id: UPX UPXV ", 14);
    checkPatch(b, blen, boff, 14);

    unsigned char *p = (unsigned char *)b + boff + 9;
    memset(p, ' ', 4);
    memcpy(p, UPX_VERSION_STRING, UPX_MIN(strlen(UPX_VERSION_STRING), 4));

    return boff;
}


/*************************************************************************
// relocation util
**************************************************************************/

upx_byte *Packer::optimizeReloc32(upx_byte *in, unsigned relocnum,
                                  upx_byte *out, upx_byte *image,
                                  int bswap, int *big)
{
#if defined(UNUPX)
    return out;
#else
    *big = 0;
    if (relocnum == 0)
        return out;
    qsort(in,relocnum,4,le32_compare);

    unsigned jc,pc,oc;
    upx_byte *fix = out;

    pc = (unsigned) -4;
    for (jc = 0; jc<relocnum; jc++)
    {
        oc = get_le32(in+jc*4) - pc;
        if (oc == 0)
            continue;
        else if ((int)oc < 4)
            throwCantPack("overlapping fixups");
        else if (oc < 0xF0)
            *fix++ = (unsigned char) oc;
        else if (oc < 0x100000)
        {
            *fix++ = (unsigned char) (0xF0+(oc>>16));
            *fix++ = (unsigned char) oc;
            *fix++ = (unsigned char) (oc>>8);
        }
        else
        {
            *big = 1;
            *fix++ = 0xf0;
            *fix++ = 0;
            *fix++ = 0;
            set_le32(fix,oc);
            fix += 4;
        }
        pc += oc;
        if (bswap)
            set_be32(image+pc,get_le32(image+pc));
    }
    *fix++ = 0;
    return fix;
#endif /* UNUPX */
}


unsigned Packer::unoptimizeReloc32(upx_byte **in, upx_byte *image,
                                   MemBuffer *out, int bswap)
{
    upx_byte *p;
    unsigned relocn = 0;
    for (p = *in; *p; p++, relocn++)
        if (*p >= 0xF0)
        {
            if (*p == 0xF0 && get_le16(p+1) == 0)
                p += 4;
            p += 2;
        }
    //fprintf(stderr,"relocnum=%x\n",relocn);
    out->alloc(4*relocn+4); // one extra data
    LE32 *outp = (LE32*) (unsigned char *) *out;
    LE32 *relocs = outp;
    unsigned jc = (unsigned) -4;
    for (p = *in; *p; p++)
    {
        if (*p < 0xF0)
            jc += *p;
        else
        {
            unsigned dif = (*p & 0x0F)*0x10000 + get_le16(p+1);
            p += 2;
            if (dif == 0)
            {
                dif = get_le32(p+1);
                p += 4;
            }
            jc += dif;
        }
        *relocs++ = jc;
        if (bswap && image)
            set_be32(image+jc,get_le32(image+jc));
    }
    //fprintf(stderr,"relocnum=%x\n",relocn);
    *in = p+1;
    return relocs - outp;
}


/*************************************************************************
// loader util
**************************************************************************/

void Packer::initLoader(const void *pdata, int plen, int pinfo)
{
    if (pinfo < 0)
        pinfo =  ~3 & (3 + get_le16(pdata, plen - 2));

    delete linker;
    if (getFormat() < 128)
        linker = new Linker(pdata, plen, pinfo);    // little endian
    else
        linker = new BeLinker(pdata, plen, pinfo);  // big endian

    static const char identbig[] =
        "\n\0"
        "$Info: This file is packed with the UPX executable packer http://upx.tsx.org $"
        "\n\0"
        "$Id: UPX " UPX_VERSION_STRING " Copyright (C) 1996-2001 the UPX Team. All Rights Reserved. $"
        "\n";

    static const char identsmall[] =
        "\n"
        "$Id: UPX (C) 1996-2001 the UPX Team. All Rights Reserved. http://upx.tsx.org $"
        "\n";

    if (opt->small)
        linker->addSection("IDENTSTR",identsmall,sizeof(identsmall));
    else
        linker->addSection("IDENTSTR",identbig,sizeof(identbig));
}


void Packer::addLoader(const char *s, ...)
{
    const char *p;
    va_list ap;

    linker->addSection(s);
    va_start(ap, s);
    while((p = va_arg(ap, const char *)) != NULL)
        linker->addSection(p);
    va_end(ap);
}


int Packer::getLoaderSection(const char *name, int *slen) const
{
    int size = -1;
    int ostart = linker->getSection(name, &size);
    if (ostart < 0 || size <= 0)
        throwBadLoader();
    if (slen)
        *slen = size;
    return ostart;
}


// same, but the size of the section may be == 0
int Packer::getLoaderSectionStart(const char *name, int *slen) const
{
    int size = -1;
    int ostart = linker->getSection(name, &size);
    if (ostart < 0 || size < 0)
        throwBadLoader();
    if (slen)
        *slen = size;
    return ostart;
}


const upx_byte *Packer::getLoader() const
{
    int size = -1;
    const char *oloader = linker->getLoader(&size);
    if (oloader == NULL || size <= 0)
        throwBadLoader();
    return (const upx_byte *) oloader;
}


int Packer::getLoaderSize() const
{
    int size = -1;
    const char *oloader = linker->getLoader(&size);
    if (oloader == NULL || size <= 0)
        throwBadLoader();
    return size;
}


const char *Packer::getDecompressor() const
{
    static const char nrv2b_le32_small[] =
        "N2BSMA10""N2BDEC10""N2BSMA20""N2BDEC20""N2BSMA30"
        "N2BDEC30""N2BSMA40""N2BSMA50""N2BDEC50""N2BSMA60"
        "N2BDEC60";

    static const char nrv2b_le32_fast[] =
        "N2BFAS10""+80CXXXX""N2BFAS11""N2BDEC10""N2BFAS20"
        "N2BDEC20""N2BFAS30""N2BDEC30""N2BFAS40""N2BFAS50"
        "N2BDEC50""N2BFAS60""+40CXXXX""N2BFAS61""N2BDEC60";

    static const char nrv2d_le32_small[] =
        "N2DSMA10""N2DDEC10""N2DSMA20""N2DDEC20""N2DSMA30"
        "N2DDEC30""N2DSMA40""N2DSMA50""N2DDEC50""N2DSMA60"
        "N2DDEC60";

    static const char nrv2d_le32_fast[] =
        "N2DFAS10""+80CXXXX""N2DFAS11""N2DDEC10""N2DFAS20"
        "N2DDEC20""N2DFAS30""N2DDEC30""N2DFAS40""N2DFAS50"
        "N2DDEC50""N2DFAS60""+40CXXXX""N2DFAS61""N2DDEC60";

    if (ph.method == M_NRV2B_LE32)
        return opt->small ? nrv2b_le32_small : nrv2b_le32_fast;
    if (ph.method == M_NRV2D_LE32)
        return opt->small ? nrv2d_le32_small : nrv2d_le32_fast;
    throwInternalError("bad decompressor");
    return NULL;
}


#define NOFILT 0  // no filter
#define FNOMRU 1  // filter, but not using mru
#define MRUFLT 2  // mru filter

static unsigned
f80_call(int fid)
{
    return (1+ (0x0f & fid)) % 3;
}

static unsigned
f80_jmp1(int fid)
{
    return ((1+ (0x0f & fid)) / 3) % 3;
}

static unsigned
f80_jcc2(int fid)
{
    return f80_jmp1(fid);
}

void Packer::addFilter32(int filter_id)
{
    assert(filter_id > 0);
    assert(isValidFilter(filter_id));

    if (filter_id < 0x80) {
        if ((filter_id & 0xf) % 3 == 0) {
            if (filter_id < 0x40) {
                addLoader("CALLTR00",
                        (filter_id > 0x20) ? "CTCLEVE1" : "",
                        "CALLTR01",
                        (filter_id & 0xf) > 3 ? (filter_id > 0x20 ? "CTBSHR01""CTBSWA01" : "CTBROR01""CTBSWA01") : "",
                        "CALLTR02",
                        NULL
                        );
            }
            else if (0x40==(0xF0 & filter_id)) {
                addLoader("CKLLTR00", 0);
                if (9<=(0xf & filter_id)) {
                    addLoader("CKLLTR10", 0);
                }
                addLoader("CKLLTR20", 0);
                if (9<=(0xf & filter_id)) {
                    addLoader("CKLLTR30", 0);
                }
                addLoader("CKLLTR40", 0);
            }
        }
        else
            addLoader("CALLTR10",
                    (filter_id & 0xf) % 3  == 1 ? "CALLTRE8" : "CALLTRE9",
                    "CALLTR11",
                    (filter_id > 0x20) ? "CTCLEVE2" : "",
                    "CALLTR12",
                    (filter_id & 0xf) > 3 ? (filter_id > 0x20 ? "CTBSHR11""CTBSWA11" : "CTBROR11""CTBSWA11") : "",
                    "CALLTR13",
                    NULL
                    );
    }
    if (0x80==(filter_id & 0xF0)) {
        bool const x386 = (opt->cpu <= opt->CPU_386);
        unsigned const n_mru = ph.n_mru ? 1+ ph.n_mru : 0;
        bool const mrupwr2 = (0!=n_mru) && 0==((n_mru -1) & n_mru);
        unsigned const f_call = f80_call(filter_id);
        unsigned const f_jmp1 = f80_jmp1(filter_id);
        unsigned const f_jcc2 = f80_jcc2(filter_id);

        if (NOFILT!=f_jcc2) {
	        addLoader("LXJCC010", 0);
	        if (n_mru) {
	            addLoader("LXMRU045", 0);
	        }
	        else {
	            addLoader("LXMRU046", 0);
	        }
	        if (0==n_mru || MRUFLT!=f_jcc2) {
	            addLoader("LXJCC020", 0);
	        }
	        else { // 0!=n_mru
	            addLoader("LXJCC021", 0);
	        }
	        if (NOFILT!=f_jcc2) {
	            addLoader("LXJCC023", 0);
	        }
        }
        addLoader("LXUNF037", 0);
        if (x386) {
            if (n_mru) {
                addLoader("LXUNF386", 0);
            }
            addLoader("LXUNF387", 0);
            if (n_mru) {
                addLoader("LXUNF388", 0);
            }
        }
        else {
            addLoader("LXUNF486", 0);
            if (n_mru) {
                addLoader("LXUNF487", 0);
            }
        }
        if (n_mru) {
            addLoader("LXMRU065", 0);
            if (256==n_mru) {
                addLoader("MRUBYTE3", 0);
            }
            else {
                addLoader("MRUARB30", 0);
                if (mrupwr2) {
                    addLoader("MRUBITS3", 0);
                }
                else {
                    addLoader("MRUARB40", 0);
                }
            }
            addLoader("LXMRU070", 0);
            if (256==n_mru) {
                addLoader("MRUBYTE4", 0);
            }
            else if (mrupwr2) {
                addLoader("MRUBITS4", 0);
            }
            else {
                addLoader("MRUARB50", 0);
            }
            addLoader("LXMRU080", 0);
            if (256==n_mru) {
                addLoader("MRUBYTE5", 0);
            }
            else {
                addLoader("MRUARB60", 0);
                if (mrupwr2) {
                    addLoader("MRUBITS5", 0);
                }
                else {
                    addLoader("MRUARB70", 0);
                }
            }
            addLoader("LXMRU090", 0);
            if (256==n_mru) {
                addLoader("MRUBYTE6", 0);
            }
            else {
                addLoader("MRUARB80", 0);
                if (mrupwr2) {
                        addLoader("MRUBITS6", 0);
                }
                else {
                    addLoader("MRUARB90", 0);
                }
            }
            addLoader("LXMRU100", 0);
        }
        addLoader("LXUNF040", 0);
        if (n_mru) {
            addLoader("LXMRU110", 0);
        }
        else {
            addLoader("LXMRU111", 0);
        }
        addLoader("LXUNF041", 0);
        addLoader("LXUNF042", 0);
        if (n_mru) {
            addLoader("LXMRU010", 0);
            if (NOFILT!=f_jmp1 && NOFILT==f_call) {
                addLoader("LXJMPA00", 0);
            }
            else {
                addLoader("LXCALLB0", 0);
            }
            addLoader("LXUNF021", 0);
        }
        else {
            addLoader("LXMRU022", 0);
            if (NOFILT!=f_jmp1 && NOFILT==f_call) {
                addLoader("LXJMPA01", 0);
            }
            else {
                addLoader("LXCALLB1", 0);
            }
        }
        if (n_mru) {    
            if (256!=n_mru && mrupwr2) {
                addLoader("MRUBITS1", 0);
            }
            addLoader("LXMRU030", 0);
            if (256==n_mru) {
                addLoader("MRUBYTE1", 0);
            }
            else {
                addLoader("MRUARB10", 0);
            }
            addLoader("LXMRU040", 0);
        }

        addLoader("LXUNF030", 0);
        if (NOFILT!=f_jcc2) {
            addLoader("LXJCC000", 0);
        }
        if (NOFILT!=f_call || NOFILT!=f_jmp1) { // at least one is filtered
            // shift opcode origin to zero
            if (0==n_mru) {
                addLoader("LXCJ0MRU", 0);
            }
            else {
                addLoader("LXCJ1MRU", 0);
            }
    
            // determine if in range
            if ((NOFILT!=f_call) && (NOFILT!=f_jmp1)) { // unfilter both
                addLoader("LXCALJMP", 0);
            }
            if ((NOFILT==f_call) ^  (NOFILT==f_jmp1)) { // unfilter just one
                if (0==n_mru) {
                    addLoader("LXCALL00", 0);
                }
                else {
                    addLoader("LXCALL01", 0);
                }
            }
    
            // determine if mru applies
            if (0==n_mru || ! ((FNOMRU==f_call) || (FNOMRU==f_jmp1)) ) {
                addLoader("LXCJ2MRU", 0);  // no mru, or no exceptions
            }
            else {  // mru on one, but not the other
                addLoader("LXCJ4MRU", 0);
                if (MRUFLT==f_jmp1) { // JMP only
                    addLoader("LXCJ6MRU", 0);
                } else
                if (MRUFLT==f_call) { // CALL only
                    addLoader("LXCJ7MRU", 0);
                }
                addLoader("LXCJ8MRU", 0);
            }
        }
        addLoader("LXUNF034", 0);
        if (n_mru) {
            addLoader("LXMRU055", 0);
            if (256==n_mru) {
                addLoader("MRUBYTE2", 0);
            }
            else if (mrupwr2) {
                addLoader("MRUBITS2", 0);
            }
            else if (n_mru) {
                addLoader("MRUARB20", 0);
            }
            addLoader("LXMRU057", 0);
        }
    }
}


/*************************************************************************
// Try compression with several methods and filters, choose the best
/  or first working one. Needs buildLoader().
//
// Required inputs:
//   this->ph
//     ulen
//   parm_ft
//     clevel
//     addvalue
//     buf_len (optional)
//
// - updates this->ph
// - updates *ft
// - ibuf[] is restored to the original unfiltered version
// - obuf[] contains the best compressed version
//
// strategy:
//   n:  try the first N filters, use best one
//  -1:  try all filters, use first working one
//  -2:  try only the opt->filter filter
//  -3:  use no filter at all
//
// This has been prepared for generalization into class Packer so that
// opt->all_methods and/or opt->all_filters are available for all
// executable formats.
//
// It will replace the tryFilters() / compress() call sequence.
**************************************************************************/

void Packer::compressWithFilters(Filter *parm_ft,
                                 const unsigned overlap_range,
                                 int strategy, const int *parm_filters,
                                 unsigned max_offset, unsigned max_match,
                                 unsigned filter_off, unsigned compress_buf_off)
{
    const int *f;
    //
    const PackHeader orig_ph = this->ph;
          PackHeader best_ph = this->ph;
    //
    const Filter orig_ft = *parm_ft;
          Filter best_ft = *parm_ft;
    //
    const unsigned compress_buf_len = orig_ph.u_len;
    const unsigned filter_len = orig_ft.buf_len ? orig_ft.buf_len : compress_buf_len;
    //
    best_ph.c_len = orig_ph.u_len;
    best_ph.overlap_overhead = 0;
    unsigned best_ph_lsize = 0;

    // preconditions
    assert(orig_ph.filter == 0);
    assert(orig_ft.id == 0);
    assert(filter_off + filter_len <= compress_buf_off + compress_buf_len);

    // setup filter strategy
    if (strategy == 0)
    {
        if (opt->all_filters)
            // choose best from all available filters
            strategy = INT_MAX;
        else if (opt->filter >= 0 && isValidFilter(opt->filter))
            // try opt->filter
            strategy = -2;
        else
            // try the first working filter
            strategy = -1;
    }
    assert(strategy != 0);

    // setup raw_filters
    int tmp_filters[] = { 0, -1 };
    const int *raw_filters = NULL;
    if (strategy == -3)
        raw_filters = tmp_filters;
    else if (strategy == -2 && opt->filter >= 0)
    {
        tmp_filters[0] = opt->filter;
        raw_filters = tmp_filters;
    }
    else
    {
        raw_filters = parm_filters;
        if (raw_filters == NULL)
            raw_filters = getFilters();
        if (raw_filters == NULL)
            raw_filters = tmp_filters;
    }

    // first pass - count number of filters
    int raw_nfilters = 0;
    for (f = raw_filters; *f >= 0; f++)
    {
        assert(isValidFilter(*f));
        raw_nfilters++;
    }

    // copy filters, add a 0
    int nfilters = 0;
    bool zero_seen = false;
    autoheap_array(int, filters, raw_nfilters + 2);
    for (f = raw_filters; *f >= 0; f++)
    {
        if (*f == 0)
            zero_seen = true;
        filters[nfilters++] = *f;
        if (nfilters == strategy)
            break;
    }
    if (!zero_seen)
        filters[nfilters++] = 0;
    filters[nfilters] = -1;

    // methods
    int tmp_methods[] = { ph.method, -1 };
    const int *methods = NULL;
    if (opt->all_methods)
        methods = getCompressionMethods(-1, ph.level);
    if (methods == NULL)
        methods = tmp_methods;
    int nmethods = 0;
    while (methods[nmethods] >= 0)
    {
        assert(isValidCompressionMethod(methods[nmethods]));
        nmethods++;
    }
    assert(nmethods > 0);

    // update total_passes
    if (strategy < 0)
        ui_total_passes += 1 * nmethods;
    else
        ui_total_passes += nfilters * nmethods;

    // Working buffer for compressed data. Don't waste memory.
    upx_byte *otemp = obuf;
    MemBuffer otemp_buf;

    // compress
    int nfilters_success = 0;
    for (int m = 0; m < nmethods; m++)          // for all methods
    {
        for (int i = 0; i < nfilters; i++)          // for all filters
        {
            // get fresh packheader
            ph = orig_ph;
            ph.method = methods[m];
            ph.filter = filters[i];
            // get fresh filter
            Filter ft = orig_ft;
            ft.init(ph.filter, orig_ft.addvalue);
            // filter
            optimizeFilter(&ft, ibuf + filter_off, filter_len);

    unsigned char *const save = new unsigned char[filter_len];
    memcpy(save, ibuf + filter_off, filter_len);

            bool success = ft.filter(ibuf + filter_off, filter_len);
            if (ft.id != 0 && ft.calls == 0)
            {
                // filter did not do anything - no need to call ft.unfilter()
                success = false;
            }
            if (!success)
            {
                // filter failed or was usesless
                if (strategy > 0)
                {
                    // adjust passes
                    if (ui_pass >= 0)
                        ui_pass++;
                }
                continue;
            }
            // filter success
#if 0
            printf("filter: id 0x%02x size %6d, calls %5d/%5d/%3d/%5d/%5d, cto 0x%02x\n",
                   ft.id, ft.buf_len, ft.calls, ft.noncalls, ft.wrongcalls, ft.firstcall, ft.lastcall, ft.cto);
#endif
            if (nfilters_success > 0 && otemp == obuf)
            {
                otemp_buf.allocForCompression(compress_buf_len);
                otemp = otemp_buf;
            }
            nfilters_success++;
            ph.filter_cto = ft.cto;
            ph.n_mru = ft.n_mru;
            // compress
            if (compress(ibuf + compress_buf_off, otemp, max_offset, max_match))
            {
                unsigned lsize = 0;
                if (ph.c_len + lsize < best_ph.c_len + best_ph_lsize)
                {
                    // get results
                    ph.overlap_overhead = findOverlapOverhead(otemp, overlap_range);
                    lsize = buildLoader(&ft);
                }
#if 0
                printf("\n%2d %02x: %d +%4d = %d  (best: %d +%4d = %d)\n", ph.method, ph.filter,
                       ph.c_len, lsize, ph.c_len + lsize,
                       best_ph.c_len, best_ph_lsize, best_ph.c_len + best_ph_lsize);
#endif
                bool update = false;
                if (ph.c_len + lsize < best_ph.c_len + best_ph_lsize)
                    update = true;
                else if (ph.c_len + lsize == best_ph.c_len + best_ph_lsize)
                {
                    // prefer smaller loaders
                    if (lsize < best_ph_lsize)
                        update = true;
                    else if (lsize == best_ph_lsize)
                    {
                        // prefer less overlap_overhead
                        if (ph.overlap_overhead < best_ph.overlap_overhead)
                            update = true;
                    }
                }
                if (update)
                {
                    // update obuf[] with best version
                    if (otemp != obuf)
                        memcpy(obuf, otemp, ph.c_len);
                    // save compression results
                    best_ph = ph;
                    best_ph_lsize = lsize;
                    best_ft = ft;
                }
            }
            // restore ibuf[] - unfilter with verify
            ft.unfilter(ibuf + filter_off, filter_len, true);
        for (unsigned k = 0; k < filter_len; ++k) {
            if ((ibuf + filter_off)[k] != save[k]) {
                printf("mismatch at %d\n", k);
            }
        }
            //
            if (strategy < 0)
                break;
        }
    }

    // postconditions 1)
    assert(nfilters_success > 0);
    assert(best_ph.u_len == orig_ph.u_len);
    assert(best_ph.filter == best_ft.id);
    assert(best_ph.filter_cto == best_ft.cto);
    // FIXME  assert(best_ph.n_mru == best_ft.n_mru);

    // copy back results
    this->ph = best_ph;
    *parm_ft = best_ft;

    // finally check compression ratio
    if (best_ph.c_len + best_ph_lsize >= best_ph.u_len)
        throwNotCompressible();
    if (!checkCompressionRatio(best_ph.u_len, best_ph.c_len))
        throwNotCompressible();

    // postconditions 2)
    assert(best_ph.overlap_overhead > 0);

    // convenience
    buildLoader(&best_ft);
}


/*************************************************************************
// filter util
**************************************************************************/

bool Packer::isValidFilter(int filter_id) const
{
    if (!Filter::isValidFilter(filter_id))
        return false;
    if (filter_id == 0)
        return true;
    for (const int *f = getFilters(); f && *f >= 0; f++)
    {
        if (*f == filter_id)
            return true;
    }
    return false;
}


void Packer::tryFilters(Filter *ft, upx_byte *buf, unsigned buf_len,
                        unsigned addvalue) const
{
    // debug
    //scanFilters(ft, buf, buf_len, addvalue);

    ft->init();
    if (opt->filter == 0)
        return;
    for (const int *f = getFilters(); f && *f >= 0; f++)
    {
        if (*f == 0)        // skip no-filter
            continue;
        if (opt->filter < 0 || *f == opt->filter)
        {
            ft->init(*f, addvalue);
            optimizeFilter(ft, buf, buf_len);
            if (ft->filter(buf, buf_len) && ft->calls > 0)
                break;                      // success
            ft->init();
        }
    }
}


void Packer::scanFilters(Filter *ft, const upx_byte *buf, unsigned buf_len,
                         unsigned addvalue) const
{
    ft->init();
    if (opt->filter == 0)
        return;
    for (const int *f = getFilters(); f && *f >= 0; f++)
    {
        if (*f == 0)        // skip no-filter
            continue;
        ft->init(*f, addvalue);
        //static const int ctos[] = { 0xff, 0xfe, 0x80, 0x22, -1 };
        //ft->preferred_ctos = ctos;
        if (ft->scan(buf, buf_len))
        {
            printf("scanFilters: id 0x%02x size: %6d: calls %5d/%5d/%3d, cto 0x%02x\n",
                   ft->id, ft->buf_len, ft->calls, ft->noncalls, ft->wrongcalls, ft->cto);
        }
        ft->init();
    }
}


bool Packer::patchFilter32(void *loader, int lsize, const Filter *ft)
{
    if (ft->id == 0)
        return false;
    assert(ft->calls > 0);
    if (ft->id < 0x80) {
        if (0x40 <= ft->id && ft->id < 0x50 && UPX_F_LINUX_i386==ph.format) {
            // "push byte '?'"
            patch_le16(loader, lsize, "\x6a?", 0x6a + (ft->cto << 8));
        }
        if (0x20 <= ft->id && ft->id < 0x40) {
            // 077==modr/m of "cmp [edi], byte '?'" (compare immediate 8 bits)
            patch_le16(loader, lsize, "\077?", 077 + (ft->cto << 8));
        }
        if (ft->id < 0x40) {
            patch_le32(loader, lsize, "TEXL", (ft->id & 0xf) % 3 == 0 ? ft->calls :
                    ft->lastcall - ft->calls * 4);
        }
    }
    if (0x80==(ft->id & 0xF0)) {
        int const mru = ph.n_mru ? 1+ ph.n_mru : 0;
        if (mru && mru!=256) {
            unsigned const is_pwr2 = (0==((mru -1) & mru));
            patch_le32(0x80 + (char *)loader, lsize - 0x80, "NMRU", mru - is_pwr2);
        }
    }
    return true;
}


/*
vi:ts=4:et:nowrap
*/

