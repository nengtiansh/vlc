/*****************************************************************************
 * video.c: video decoder using the ffmpeg library
 *****************************************************************************
 * Copyright (C) 1999-2001 VideoLAN
 * $Id: video.c,v 1.61 2004/01/18 21:30:25 fenrir Exp $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@netcourrier.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>
#include <vlc/decoder.h>

/* ffmpeg header */
#ifdef HAVE_FFMPEG_AVCODEC_H
#   include <ffmpeg/avcodec.h>
#else
#   include <avcodec.h>
#endif

#include "ffmpeg.h"

/*****************************************************************************
 * decoder_sys_t : decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
    /* Common part between video and audio decoder */
    int i_cat;
    int i_codec_id;
    char *psz_namecodec;

    AVCodecContext      *p_context;
    AVCodec             *p_codec;

    /* Video decoder specific part */
    mtime_t input_pts;
    mtime_t input_dts;
    mtime_t i_pts;

    AVFrame          *p_ff_pic;
    BITMAPINFOHEADER *p_format;

    /* for frame skipping algo */
    int b_hurry_up;
    int i_frame_skip;

    /* how many decoded frames are late */
    int     i_late_frames;
    mtime_t i_late_frames_start;

    /* for direct rendering */
    int b_direct_rendering;

    vlc_bool_t b_has_b_frames;

    int i_buffer_orig, i_buffer;
    char *p_buffer_orig, *p_buffer;

    /* Postprocessing handle */
    void *p_pp;
    vlc_bool_t b_pp;
    vlc_bool_t b_pp_async;
    vlc_bool_t b_pp_init;
};

/* FIXME (dummy palette for now) */
static AVPaletteControl palette_control;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void ffmpeg_CopyPicture    ( decoder_t *, picture_t *, AVFrame * );
static int  ffmpeg_GetFrameBuf    ( struct AVCodecContext *, AVFrame * );
static void ffmpeg_ReleaseFrameBuf( struct AVCodecContext *, AVFrame * );

/*****************************************************************************
 * Local Functions
 *****************************************************************************/
static uint32_t ffmpeg_PixFmtToChroma( int i_ff_chroma )
{
    switch( i_ff_chroma )
    {
    case PIX_FMT_YUV420P:
        return VLC_FOURCC('I','4','2','0');
    case PIX_FMT_YUV422P:
        return VLC_FOURCC('I','4','2','2');
    case PIX_FMT_YUV444P:
        return VLC_FOURCC('I','4','4','4');

    case PIX_FMT_YUV422:
        return VLC_FOURCC('Y','U','Y','2');

    case PIX_FMT_RGB555:
        return VLC_FOURCC('R','V','1','5');
    case PIX_FMT_RGB565:
        return VLC_FOURCC('R','V','1','6');
    case PIX_FMT_RGB24:
        return VLC_FOURCC('R','V','2','4');
    case PIX_FMT_RGBA32:
        return VLC_FOURCC('R','V','3','2');
    case PIX_FMT_GRAY8:
        return VLC_FOURCC('G','R','E','Y');

    case PIX_FMT_YUV410P:
    case PIX_FMT_YUV411P:
    case PIX_FMT_BGR24:
    default:
        return 0;
    }
}

/* Returns a new picture buffer */
static inline picture_t *ffmpeg_NewPictBuf( decoder_t *p_dec,
                                            AVCodecContext *p_context )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    picture_t *p_pic;

    p_dec->fmt_out.video.i_width = p_context->width;
    p_dec->fmt_out.video.i_height = p_context->height;
    p_dec->fmt_out.i_codec = ffmpeg_PixFmtToChroma( p_context->pix_fmt );

    if( !p_context->width || !p_context->height )
    {
        return NULL; /* invalid display size */
    }

    if( !p_dec->fmt_out.i_codec )
    {
        /* we make conversion if possible*/
        p_dec->fmt_out.i_codec = VLC_FOURCC('I','4','2','0');
    }

    /* If an aspect-ratio was specified in the input format then force it */
    if( p_dec->fmt_in.video.i_aspect )
    {
        p_dec->fmt_out.video.i_aspect = p_dec->fmt_in.video.i_aspect;
    }
    else
    {
#if LIBAVCODEC_BUILD >= 4687
        p_dec->fmt_out.video.i_aspect =
            VOUT_ASPECT_FACTOR * ( av_q2d(p_context->sample_aspect_ratio) *
                p_context->width / p_context->height );
#else
        p_dec->fmt_out.video.i_aspect =
            VOUT_ASPECT_FACTOR * p_context->aspect_ratio;
#endif
        if( p_dec->fmt_out.video.i_aspect == 0 )
        {
            p_dec->fmt_out.video.i_aspect =
                VOUT_ASPECT_FACTOR * p_context->width / p_context->height;
        }
    }

    p_pic = p_dec->pf_vout_buffer_new( p_dec );

#ifdef LIBAVCODEC_PP
    if( p_sys->p_pp && p_sys->b_pp && !p_sys->b_pp_init )
    {
        E_(InitPostproc)( p_dec, p_sys->p_pp, p_context->width,
                          p_context->height, p_context->pix_fmt );
        p_sys->b_pp_init = VLC_TRUE;
    }
#endif

    return p_pic;
}

/*****************************************************************************
 * InitVideo: initialize the video decoder
 *****************************************************************************
 * the ffmpeg codec will be opened, some memory allocated. The vout is not yet
 * opened (done after the first decoded frame).
 *****************************************************************************/
int E_(InitVideoDec)( decoder_t *p_dec, AVCodecContext *p_context,
                      AVCodec *p_codec, int i_codec_id, char *psz_namecodec )
{
    decoder_sys_t *p_sys;
    vlc_value_t lockval;
    vlc_value_t val;
    int i_tmp;


    var_Get( p_dec->p_libvlc, "avcodec", &lockval );

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys =
          (decoder_sys_t *)malloc(sizeof(decoder_sys_t)) ) == NULL )
    {
        msg_Err( p_dec, "out of memory" );
        return VLC_EGENERIC;
    }

    p_dec->p_sys->p_context = p_context;
    p_dec->p_sys->p_codec = p_codec;
    p_dec->p_sys->i_codec_id = i_codec_id;
    p_dec->p_sys->psz_namecodec = psz_namecodec;
    p_sys->p_ff_pic = avcodec_alloc_frame();

    /* ***** Fill p_context with init values ***** */
    p_sys->p_context->width  = p_dec->fmt_in.video.i_width;
    p_sys->p_context->height = p_dec->fmt_in.video.i_height;

    /*  ***** Get configuration of ffmpeg plugin ***** */
    i_tmp = config_GetInt( p_dec, "ffmpeg-workaround-bugs" );
    p_sys->p_context->workaround_bugs  = __MAX( __MIN( i_tmp, 99 ), 0 );

    i_tmp = config_GetInt( p_dec, "ffmpeg-error-resilience" );
    p_sys->p_context->error_resilience = __MAX( __MIN( i_tmp, 99 ), -1 );

    var_Create( p_dec, "grayscale", VLC_VAR_BOOL | VLC_VAR_DOINHERIT );
    var_Get( p_dec, "grayscale", &val );
    if( val.b_bool ) p_sys->p_context->flags |= CODEC_FLAG_GRAY;

    /* Decide if we set CODEC_FLAG_TRUNCATED */
    var_Create( p_dec, "ffmpeg-truncated", VLC_VAR_INTEGER|VLC_VAR_DOINHERIT );
    var_Get( p_dec, "ffmpeg-truncated", &val );
    if( val.i_int > 0 ) p_sys->p_context->flags |= CODEC_FLAG_TRUNCATED;

    /* ***** ffmpeg frame skipping ***** */
    var_Create( p_dec, "ffmpeg-hurry-up", VLC_VAR_BOOL | VLC_VAR_DOINHERIT );
    var_Get( p_dec, "ffmpeg-hurry-up", &val );
    p_sys->b_hurry_up = val.b_bool;

    /* ***** ffmpeg direct rendering ***** */
    p_sys->b_direct_rendering = 0;
    var_Create( p_dec, "ffmpeg-dr", VLC_VAR_BOOL | VLC_VAR_DOINHERIT );
    var_Get( p_dec, "ffmpeg-dr", &val );
    if( val.b_bool && (p_sys->p_codec->capabilities & CODEC_CAP_DR1) &&
        ffmpeg_PixFmtToChroma( p_sys->p_context->pix_fmt ) &&
        /* Apparently direct rendering doesn't work with YUV422P */
        p_sys->p_context->pix_fmt != PIX_FMT_YUV422P &&
        !(p_sys->p_context->width % 16) && !(p_sys->p_context->height % 16) )
    {
        /* Some codecs set pix_fmt only after the 1st frame has been decoded,
         * so we need to do another check in ffmpeg_GetFrameBuf() */
        p_sys->b_direct_rendering = 1;
    }

#ifdef LIBAVCODEC_PP
    p_sys->p_pp = NULL;
    p_sys->b_pp = p_sys->b_pp_async = p_sys->b_pp_init = VLC_FALSE;
    p_sys->p_pp = E_(OpenPostproc)( p_dec, &p_sys->b_pp_async );
#endif

    /* ffmpeg doesn't properly release old pictures when frames are skipped */
    //if( p_sys->b_hurry_up ) p_sys->b_direct_rendering = 0;
    if( p_sys->b_direct_rendering )
    {
        msg_Dbg( p_dec, "using direct rendering" );
        p_sys->p_context->flags |= CODEC_FLAG_EMU_EDGE;
    }

    /* Always use our get_buffer wrapper so we can calculate the
     * PTS correctly */
    p_sys->p_context->get_buffer = ffmpeg_GetFrameBuf;
    p_sys->p_context->release_buffer = ffmpeg_ReleaseFrameBuf;
    p_sys->p_context->opaque = p_dec;

    /* ***** init this codec with special data ***** */
    if( p_dec->fmt_in.i_extra )
    {
        int i_size = p_dec->fmt_in.i_extra;

        if( p_sys->i_codec_id == CODEC_ID_SVQ3 )
        {
            uint8_t *p;

            p_sys->p_context->extradata_size = i_size + 12;
            p = p_sys->p_context->extradata  =
                malloc( p_sys->p_context->extradata_size );

            memcpy( &p[0],  "SVQ3", 4 );
            memset( &p[4], 0, 8 );
            memcpy( &p[12], p_dec->fmt_in.p_extra, i_size );
        }
        else if( p_dec->fmt_in.i_codec == VLC_FOURCC( 'R', 'V', '1', '0' ) ||
                 p_dec->fmt_in.i_codec == VLC_FOURCC( 'R', 'V', '1', '3' ) ||
                 p_dec->fmt_in.i_codec == VLC_FOURCC( 'R', 'V', '2', '0' ) )
        {
            if( p_dec->fmt_in.i_extra == 8 )
            {
                p_sys->p_context->extradata_size = 8;
                p_sys->p_context->extradata = malloc( 8 );

                memcpy( p_sys->p_context->extradata,
                        p_dec->fmt_in.p_extra,
                        p_dec->fmt_in.i_extra );
                p_sys->p_context->sub_id= ((uint32_t*)p_dec->fmt_in.p_extra)[1];

                msg_Warn( p_dec, "using extra data for RV codec sub_id=%08x", p_sys->p_context->sub_id );
            }
        }
        else
        {
            p_sys->p_context->extradata_size = i_size;
            p_sys->p_context->extradata =
                malloc( i_size + FF_INPUT_BUFFER_PADDING_SIZE );
            memcpy( p_sys->p_context->extradata,
                    p_dec->fmt_in.p_extra, i_size );
            memset( &((uint8_t*)p_sys->p_context->extradata)[i_size],
                    0, FF_INPUT_BUFFER_PADDING_SIZE );
        }
    }

    /* ***** misc init ***** */
    p_sys->input_pts = p_sys->input_dts = 0;
    p_sys->i_pts = 0;
    p_sys->b_has_b_frames = VLC_FALSE;
    p_sys->i_late_frames = 0;
    p_sys->i_buffer = 0;
    p_sys->i_buffer_orig = 1;
    p_sys->p_buffer_orig = p_sys->p_buffer = malloc( p_sys->i_buffer );

    /* Set output properties */
    p_dec->fmt_out.i_cat = VIDEO_ES;
    p_dec->fmt_out.i_codec = ffmpeg_PixFmtToChroma( p_context->pix_fmt );

    /* Setup dummy palette to avoid segfaults with some codecs */
#if LIBAVCODEC_BUILD >= 4688
    p_sys->p_context->palctrl = &palette_control;
#endif

    /* ***** Open the codec ***** */
    vlc_mutex_lock( lockval.p_address );
    if( avcodec_open( p_sys->p_context, p_sys->p_codec ) < 0 )
    {
        vlc_mutex_unlock( lockval.p_address );
        msg_Err( p_dec, "cannot open codec (%s)", p_sys->psz_namecodec );
        return VLC_EGENERIC;
    }
    vlc_mutex_unlock( lockval.p_address );
    msg_Dbg( p_dec, "ffmpeg codec (%s) started", p_sys->psz_namecodec );


    return VLC_SUCCESS;
}

/*****************************************************************************
 * DecodeVideo: Called to decode one or more frames
 *****************************************************************************/
picture_t *E_(DecodeVideo)( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    int b_drawpicture;
    block_t *p_block;

    if( !pp_block || !*pp_block ) return NULL;

    p_block = *pp_block;

    if( p_block->b_discontinuity )
    {
        p_sys->i_buffer = 0;
        p_sys->i_pts = 0; /* To make sure we recover properly */

        block_Release( p_block );
        return NULL;
    }

    if( p_sys->i_late_frames > 0 &&
        mdate() - p_sys->i_late_frames_start > I64C(5000000) )
    {
        if( p_sys->i_pts )
        {
            msg_Err( p_dec, "more than 5 seconds of late video -> "
                     "dropping frame (computer too slow ?)" );
            p_sys->i_pts = 0; /* To make sure we recover properly */
        }
        block_Release( p_block );
        p_sys->i_late_frames--;
        return NULL;
    }

    if( p_block->i_pts > 0 || p_block->i_dts > 0 )
    {
        p_sys->input_pts = p_block->i_pts;
        p_sys->input_dts = p_block->i_dts;

        /* Make sure we don't reuse the same timestamps twice */
        p_block->i_pts = p_block->i_dts = 0;
    }

    /* TODO implement it in a better way */
    /* A good idea could be to decode all I pictures and see for the other */
    if( p_sys->b_hurry_up && p_sys->i_late_frames > 4 )
    {
        b_drawpicture = 0;
        if( p_sys->i_late_frames < 8 )
        {
            p_sys->p_context->hurry_up = 2;
        }
        else
        {
            /* picture too late, won't decode
             * but break picture until a new I, and for mpeg4 ...*/

            p_sys->i_late_frames--; /* needed else it will never be decrease */
            block_Release( p_block );
            p_sys->i_buffer = 0;
            return NULL;
        }
    }
    else
    {
        b_drawpicture = 1;
        p_sys->p_context->hurry_up = 0;
    }


    if( p_sys->p_context->width <= 0 || p_sys->p_context->height <= 0 )
    {
        p_sys->p_context->hurry_up = 5;
    }

    /*
     * Do the actual decoding now
     */

    /* Check if post-processing was enabled */
    p_sys->b_pp = p_sys->b_pp_async;

    /* Don't forget that ffmpeg requires a little more bytes
     * that the real frame size */
    if( p_block->i_buffer > 0 )
    {
        p_sys->i_buffer = p_block->i_buffer;
        if( p_sys->i_buffer + FF_INPUT_BUFFER_PADDING_SIZE >
            p_sys->i_buffer_orig )
        {
            free( p_sys->p_buffer_orig );
            p_sys->i_buffer_orig =
                p_block->i_buffer + FF_INPUT_BUFFER_PADDING_SIZE;
            p_sys->p_buffer_orig = malloc( p_sys->i_buffer_orig );
        }
        p_sys->p_buffer = p_sys->p_buffer_orig;
        p_sys->i_buffer = p_block->i_buffer;
        p_dec->p_vlc->pf_memcpy( p_sys->p_buffer, p_block->p_buffer,
                                 p_block->i_buffer );
        memset( p_sys->p_buffer + p_block->i_buffer, 0,
                FF_INPUT_BUFFER_PADDING_SIZE );

        p_block->i_buffer = 0;
    }

    while( p_sys->i_buffer > 0 )
    {
        int i_used, b_gotpicture;
        picture_t *p_pic;

        i_used = avcodec_decode_video( p_sys->p_context, p_sys->p_ff_pic,
                                       &b_gotpicture,
                                       p_sys->p_buffer, p_sys->i_buffer );
        if( i_used < 0 )
        {
            msg_Warn( p_dec, "cannot decode one frame (%d bytes)",
                      p_sys->i_buffer );
            block_Release( p_block );
            return NULL;
        }
        else if( i_used > p_sys->i_buffer )
        {
            i_used = p_sys->i_buffer;
        }

        /* Consumed bytes */
        p_sys->i_buffer -= i_used;
        p_sys->p_buffer += i_used;

        /* Nothing to display */
        if( !b_gotpicture )
        {
            if( i_used == 0 )
            {
                break;
            }
            continue;
        }

        /* Update frame late count*/
        if( p_sys->i_pts && p_sys->i_pts <= mdate() )
        {
            p_sys->i_late_frames++;
            if( p_sys->i_late_frames == 1 )
                p_sys->i_late_frames_start = mdate();
        }
        else
        {
            p_sys->i_late_frames = 0;
        }

        if( !b_drawpicture || p_sys->p_ff_pic->linesize[0] == 0 )
        {
            /* Do not display the picture */
            continue;
        }

        if( !p_sys->b_direct_rendering || p_sys->b_pp )
        {
            /* Get a new picture */
            p_pic = ffmpeg_NewPictBuf( p_dec, p_sys->p_context );
            if( !p_pic )
            {
                block_Release( p_block );
                return NULL;
            }

            /* Fill p_picture_t from AVVideoFrame and do chroma conversion
             * if needed */
            ffmpeg_CopyPicture( p_dec, p_pic, p_sys->p_ff_pic );
        }
        else
        {
            p_pic = (picture_t *)p_sys->p_ff_pic->opaque;
        }

        /* Set the PTS */
        if( p_sys->p_ff_pic->pts ) p_sys->i_pts = p_sys->p_ff_pic->pts;

        /* Sanity check (seems to be needed for some streams ) */
        if( p_sys->p_ff_pic->pict_type == FF_B_TYPE )
        {
            p_sys->b_has_b_frames = VLC_TRUE;
        }

        /* Send decoded frame to vout */
        if( p_sys->i_pts )
        {
            p_pic->date = p_sys->i_pts;

            /* interpolate the next PTS */
            if( p_sys->p_context->frame_rate > 0 )
            {
                p_sys->i_pts += I64C(1000000) *
                    (2 + p_sys->p_ff_pic->repeat_pict) *
                    p_sys->p_context->frame_rate_base /
                    (2 * p_sys->p_context->frame_rate);
            }
            return p_pic;
        }
        else
        {
            p_dec->pf_vout_buffer_del( p_dec, p_pic );
        }
    }

    block_Release( p_block );
    return NULL;
}

/*****************************************************************************
 * EndVideo: decoder destruction
 *****************************************************************************
 * This function is called when the thread ends after a sucessful
 * initialization.
 *****************************************************************************/
void E_(EndVideoDec)( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( p_sys->p_ff_pic ) free( p_sys->p_ff_pic );

#ifdef LIBAVCODEC_PP
    E_(ClosePostproc)( p_dec, p_sys->p_pp );
#endif

    free( p_sys->p_buffer_orig );
}

/*****************************************************************************
 * ffmpeg_CopyPicture: copy a picture from ffmpeg internal buffers to a
 *                     picture_t structure (when not in direct rendering mode).
 *****************************************************************************/
static void ffmpeg_CopyPicture( decoder_t *p_dec,
                                picture_t *p_pic, AVFrame *p_ff_pic )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( ffmpeg_PixFmtToChroma( p_sys->p_context->pix_fmt ) )
    {
        int i_plane, i_size, i_line;
        uint8_t *p_dst, *p_src;
        int i_src_stride, i_dst_stride;

#ifdef LIBAVCODEC_PP
        if( p_sys->p_pp && p_sys->b_pp )
            E_(PostprocPict)( p_dec, p_sys->p_pp, p_pic, p_ff_pic );
        else
#endif
        for( i_plane = 0; i_plane < p_pic->i_planes; i_plane++ )
        {
            p_src  = p_ff_pic->data[i_plane];
            p_dst = p_pic->p[i_plane].p_pixels;
            i_src_stride = p_ff_pic->linesize[i_plane];
            i_dst_stride = p_pic->p[i_plane].i_pitch;

            i_size = __MIN( i_src_stride, i_dst_stride );
            for( i_line = 0; i_line < p_pic->p[i_plane].i_lines; i_line++ )
            {
                p_dec->p_vlc->pf_memcpy( p_dst, p_src, i_size );
                p_src += i_src_stride;
                p_dst += i_dst_stride;
            }
        }
    }
    else
    {
        AVPicture dest_pic;
        int i;

        /* we need to convert to I420 */
        switch( p_sys->p_context->pix_fmt )
        {
        case PIX_FMT_YUV410P:
        case PIX_FMT_YUV411P:
        case PIX_FMT_PAL8:
            for( i = 0; i < p_pic->i_planes; i++ )
            {
                dest_pic.data[i] = p_pic->p[i].p_pixels;
                dest_pic.linesize[i] = p_pic->p[i].i_pitch;
            }
            img_convert( &dest_pic, PIX_FMT_YUV420P,
                         (AVPicture *)p_ff_pic,
                         p_sys->p_context->pix_fmt,
                         p_sys->p_context->width,
                         p_sys->p_context->height );
            break;
        default:
            msg_Err( p_dec, "don't know how to convert chroma %i",
                     p_sys->p_context->pix_fmt );
            p_dec->b_error = 1;
            break;
        }
    }
}

/*****************************************************************************
 * ffmpeg_GetFrameBuf: callback used by ffmpeg to get a frame buffer.
 *****************************************************************************
 * It is used for direct rendering as well as to get the right PTS for each
 * decoded picture (even in indirect rendering mode).
 *****************************************************************************/
static int ffmpeg_GetFrameBuf( struct AVCodecContext *p_context,
                               AVFrame *p_ff_pic )
{
    decoder_t *p_dec = (decoder_t *)p_context->opaque;
    decoder_sys_t *p_sys = p_dec->p_sys;
    picture_t *p_pic;

    /* Set picture PTS */
    if( p_sys->input_pts )
    {
        p_ff_pic->pts = p_sys->input_pts;
    }
    else if( p_sys->input_dts )
    {
        /* Some demuxers only set the dts so let's try to find a useful
         * timestamp from this */
        if( !p_context->has_b_frames || !p_sys->b_has_b_frames ||
            !p_ff_pic->reference )
        {
            p_ff_pic->pts = p_sys->input_dts;
        }
        else p_ff_pic->pts = 0;
    }
    else p_ff_pic->pts = 0;

    p_sys->input_pts = p_sys->input_dts = 0;

    /* Not much to do in indirect rendering mode */
    if( !p_sys->b_direct_rendering || p_sys->b_pp )
    {
        return avcodec_default_get_buffer( p_context, p_ff_pic );
    }

    /* Some codecs set pix_fmt only after the 1st frame has been decoded,
     * so this check is necessary. */
    if( !ffmpeg_PixFmtToChroma( p_context->pix_fmt ) ||
        p_sys->p_context->width % 16 || p_sys->p_context->height % 16 )
    {
        msg_Dbg( p_dec, "disabling direct rendering" );
        p_sys->b_direct_rendering = 0;
        return avcodec_default_get_buffer( p_context, p_ff_pic );
    }

    /* Get a new picture */
    //p_sys->p_vout->render.b_allow_modify_pics = 0;
    p_pic = ffmpeg_NewPictBuf( p_dec, p_sys->p_context );
    if( !p_pic )
    {
        p_sys->b_direct_rendering = 0;
        return avcodec_default_get_buffer( p_context, p_ff_pic );
    }
    p_sys->p_context->draw_horiz_band = NULL;

    p_ff_pic->opaque = (void*)p_pic;
    p_ff_pic->type = FF_BUFFER_TYPE_USER;
    p_ff_pic->data[0] = p_pic->p[0].p_pixels;
    p_ff_pic->data[1] = p_pic->p[1].p_pixels;
    p_ff_pic->data[2] = p_pic->p[2].p_pixels;
    p_ff_pic->data[3] = NULL; /* alpha channel but I'm not sure */

    p_ff_pic->linesize[0] = p_pic->p[0].i_pitch;
    p_ff_pic->linesize[1] = p_pic->p[1].i_pitch;
    p_ff_pic->linesize[2] = p_pic->p[2].i_pitch;
    p_ff_pic->linesize[3] = 0;

    if( p_ff_pic->reference != 0 )
    {
        p_dec->pf_picture_link( p_dec, p_pic );
    }

    /* FIXME what is that, should give good value */
    p_ff_pic->age = 256*256*256*64; // FIXME FIXME from ffmpeg

    return 0;
}

static void ffmpeg_ReleaseFrameBuf( struct AVCodecContext *p_context,
                                    AVFrame *p_ff_pic )
{
    decoder_t *p_dec = (decoder_t *)p_context->opaque;
    picture_t *p_pic;

    if( p_ff_pic->type != FF_BUFFER_TYPE_USER )
    {
        avcodec_default_release_buffer( p_context, p_ff_pic );
        return;
    }

    p_pic = (picture_t*)p_ff_pic->opaque;

    p_ff_pic->data[0] = NULL;
    p_ff_pic->data[1] = NULL;
    p_ff_pic->data[2] = NULL;
    p_ff_pic->data[3] = NULL;

    if( p_ff_pic->reference != 0 )
    {
        p_dec->pf_picture_unlink( p_dec, p_pic );
    }
}
