/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2010 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
    
    SDL1.3 implementation by couriersud@arcor.de
    
*/
#include "SDL_config.h"

#include "SDL_DirectFB_video.h"
#include "SDL_DirectFB_render.h"
#include "../SDL_rect_c.h"
#include "../SDL_yuv_sw_c.h"

/* the following is not yet tested ... */
#define USE_DISPLAY_PALETTE			(0)

/* GDI renderer implementation */

static SDL_Renderer *DirectFB_CreateRenderer(SDL_Window * window,
                                             Uint32 flags);
static int DirectFB_DisplayModeChanged(SDL_Renderer * renderer);
static int DirectFB_ActivateRenderer(SDL_Renderer * renderer);
static int DirectFB_CreateTexture(SDL_Renderer * renderer,
                                  SDL_Texture * texture);
static int DirectFB_QueryTexturePixels(SDL_Renderer * renderer,
                                       SDL_Texture * texture,
                                       void **pixels, int *pitch);
static int DirectFB_UpdateTexture(SDL_Renderer * renderer,
                                  SDL_Texture * texture,
                                  const SDL_Rect * rect,
                                  const void *pixels, int pitch);
static int DirectFB_LockTexture(SDL_Renderer * renderer,
                                SDL_Texture * texture,
                                const SDL_Rect * rect, int markDirty,
                                void **pixels, int *pitch);
static void DirectFB_UnlockTexture(SDL_Renderer * renderer,
                                   SDL_Texture * texture);
static void DirectFB_DirtyTexture(SDL_Renderer * renderer,
                                  SDL_Texture * texture, int numrects,
                                  const SDL_Rect * rects);
static int DirectFB_RenderDrawPoints(SDL_Renderer * renderer,
                                const SDL_Point * points, int count);
static int DirectFB_RenderDrawLines(SDL_Renderer * renderer,
                               const SDL_Point * points, int count);
static int DirectFB_RenderFillRects(SDL_Renderer * renderer,
		const SDL_Rect ** rects, int count);
static int DirectFB_RenderCopy(SDL_Renderer * renderer,
                               SDL_Texture * texture,
                               const SDL_Rect * srcrect,
                               const SDL_Rect * dstrect);
static void DirectFB_RenderPresent(SDL_Renderer * renderer);
static void DirectFB_DestroyTexture(SDL_Renderer * renderer,
                                    SDL_Texture * texture);
static void DirectFB_DestroyRenderer(SDL_Renderer * renderer);

#define SDL_DFB_WINDOWSURFACE(win)  IDirectFBSurface *destsurf = ((DFB_WindowData *) ((win)->driverdata))->surface;

SDL_RenderDriver DirectFB_RenderDriver = {
    DirectFB_CreateRenderer,
    {
     "directfb",
     (SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED),
     12,
     {
      SDL_PIXELFORMAT_RGB332,
      SDL_PIXELFORMAT_RGB555,
      SDL_PIXELFORMAT_RGB565,
      SDL_PIXELFORMAT_RGB888,
      SDL_PIXELFORMAT_ARGB8888,
      SDL_PIXELFORMAT_ARGB4444,
      SDL_PIXELFORMAT_ARGB1555,
      SDL_PIXELFORMAT_RGB24,
      SDL_PIXELFORMAT_YV12,
      SDL_PIXELFORMAT_IYUV,
      SDL_PIXELFORMAT_YUY2,
      SDL_PIXELFORMAT_UYVY},
     0,
     0}
};

typedef struct
{
    SDL_Window *window;
    DFBSurfaceFlipFlags flipflags;
    int isyuvdirect;
    int size_changed;
    int lastBlendMode;
    DFBSurfaceBlittingFlags blitFlags;
    DFBSurfaceDrawingFlags drawFlags;
} DirectFB_RenderData;

typedef struct
{
    IDirectFBSurface *surface;
    Uint32 format;
    void *pixels;
    int pitch;
    SDL_VideoDisplay *display;
    SDL_DirtyRectList dirty;
#if (DFB_VERSION_ATLEAST(1,2,0))
    DFBSurfaceRenderOptions render_options;
#endif
} DirectFB_TextureData;

static __inline__ void
SDLtoDFBRect(const SDL_Rect * sr, DFBRectangle * dr)
{
    dr->x = sr->x;
    dr->y = sr->y;
    dr->h = sr->h;
    dr->w = sr->w;
}


static int
TextureHasAlpha(DirectFB_TextureData * data)
{
    /* Drawing primitive ? */
    if (!data)
        return 0;
    switch (data->format) {
    case SDL_PIXELFORMAT_ARGB4444:
    case SDL_PIXELFORMAT_ARGB1555:
    case SDL_PIXELFORMAT_ARGB8888:
    case SDL_PIXELFORMAT_RGBA8888:
    case SDL_PIXELFORMAT_ABGR8888:
    case SDL_PIXELFORMAT_BGRA8888:
    case SDL_PIXELFORMAT_ARGB2101010:
        return 1;
    default:
        return 0;
    }
}

static void
SetBlendMode(DirectFB_RenderData * data, int blendMode,
             DirectFB_TextureData * source)
{
    SDL_DFB_WINDOWSURFACE(data->window);

    //FIXME: check for format change
    if (1 || data->lastBlendMode != blendMode) {
        switch (blendMode) {
        case SDL_BLENDMODE_NONE:
                                           /**< No blending */
            data->blitFlags = DSBLIT_NOFX;
            data->drawFlags = DSDRAW_NOFX;
            SDL_DFB_CHECK(destsurf->SetSrcBlendFunction(destsurf, DSBF_ONE));
            SDL_DFB_CHECK(destsurf->SetDstBlendFunction(destsurf, DSBF_ZERO));
            break;
        case SDL_BLENDMODE_BLEND:
            data->blitFlags = DSBLIT_BLEND_ALPHACHANNEL;
            data->drawFlags = DSDRAW_BLEND;
            SDL_DFB_CHECK(destsurf->SetSrcBlendFunction(destsurf, DSBF_SRCALPHA));
            SDL_DFB_CHECK(destsurf->SetDstBlendFunction(destsurf, DSBF_INVSRCALPHA));
            break;
        case SDL_BLENDMODE_ADD:
            data->blitFlags = DSBLIT_BLEND_ALPHACHANNEL;
            data->drawFlags = DSDRAW_BLEND;
            // FIXME: SRCALPHA kills performance on radeon ...
            // It will be cheaper to copy the surface to
            // a temporay surface and premultiply 
            if (source && TextureHasAlpha(source))
                SDL_DFB_CHECK(destsurf->SetSrcBlendFunction(destsurf, DSBF_SRCALPHA));
            else
                SDL_DFB_CHECK(destsurf->SetSrcBlendFunction(destsurf, DSBF_ONE));
            SDL_DFB_CHECK(destsurf->SetDstBlendFunction(destsurf, DSBF_ONE));
            break;
        }
        data->lastBlendMode = blendMode;
    }
}

void
DirectFB_AddRenderDriver(_THIS)
{
    int i;

    for (i = 0; i < _this->num_displays; ++i) {
        SDL_AddRenderDriver(&_this->displays[i], &DirectFB_RenderDriver);
    }
}

SDL_Renderer *
DirectFB_CreateRenderer(SDL_Window * window, Uint32 flags)
{
    SDL_DFB_WINDOWDATA(window);
    SDL_VideoDisplay *display = window->display;
    SDL_Renderer *renderer = NULL;
    DirectFB_RenderData *data = NULL;
    char *p;

    SDL_DFB_CALLOC(renderer, 1, sizeof(*renderer));
    SDL_DFB_CALLOC(data, 1, sizeof(*data));

    renderer->DisplayModeChanged = DirectFB_DisplayModeChanged;
    renderer->ActivateRenderer = DirectFB_ActivateRenderer;
    renderer->CreateTexture = DirectFB_CreateTexture;
    renderer->QueryTexturePixels = DirectFB_QueryTexturePixels;
    renderer->UpdateTexture = DirectFB_UpdateTexture;
    renderer->LockTexture = DirectFB_LockTexture;
    renderer->UnlockTexture = DirectFB_UnlockTexture;
    renderer->DirtyTexture = DirectFB_DirtyTexture;
    renderer->RenderDrawPoints = DirectFB_RenderDrawPoints;
    renderer->RenderDrawLines = DirectFB_RenderDrawLines;
    renderer->RenderFillRects = DirectFB_RenderFillRects;
    /* RenderDrawEllipse - no reference implementation yet */
    /* RenderFillEllipse - no reference implementation yet */
    renderer->RenderCopy = DirectFB_RenderCopy;
    renderer->RenderPresent = DirectFB_RenderPresent;
    /* RenderReadPixels is difficult to implement */
    /* RenderWritePixels is difficult to implement */
    renderer->DestroyTexture = DirectFB_DestroyTexture;
    renderer->DestroyRenderer = DirectFB_DestroyRenderer;
    renderer->info = DirectFB_RenderDriver.info;
    renderer->window = window;      /* SDL window */
    renderer->driverdata = data;

    renderer->info.flags = SDL_RENDERER_ACCELERATED;

    data->window = window;

    data->flipflags = DSFLIP_PIPELINE | DSFLIP_BLIT;

    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        data->flipflags |= DSFLIP_WAITFORSYNC | DSFLIP_ONSYNC;
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    } else
        data->flipflags |= DSFLIP_ONSYNC;

    data->isyuvdirect = 0;      /* default is off! */
    p = SDL_getenv(DFBENV_USE_YUV_DIRECT);
    if (p)
        data->isyuvdirect = atoi(p);

    return renderer;

  error:
    SDL_DFB_FREE(renderer);
    SDL_DFB_FREE(data);
    return NULL;
}

static DFBSurfacePixelFormat
SDLToDFBPixelFormat(Uint32 format)
{
    switch (format) {
    case SDL_PIXELFORMAT_RGB332:
        return DSPF_RGB332;
    case SDL_PIXELFORMAT_RGB555:
        return DSPF_ARGB1555;
    case SDL_PIXELFORMAT_ARGB4444:
        return DSPF_ARGB4444;
    case SDL_PIXELFORMAT_ARGB1555:
        return DSPF_ARGB1555;
    case SDL_PIXELFORMAT_RGB565:
        return DSPF_RGB16;
    case SDL_PIXELFORMAT_RGB24:
        return DSPF_RGB24;
    case SDL_PIXELFORMAT_RGB888:
        return DSPF_RGB32;
    case SDL_PIXELFORMAT_ARGB8888:
        return DSPF_ARGB;
    case SDL_PIXELFORMAT_YV12:
        return DSPF_YV12;       /* Planar mode: Y + V + U  (3 planes) */
    case SDL_PIXELFORMAT_IYUV:
        return DSPF_I420;       /* Planar mode: Y + U + V  (3 planes) */
    case SDL_PIXELFORMAT_YUY2:
        return DSPF_YUY2;       /* Packed mode: Y0+U0+Y1+V0 (1 plane) */
    case SDL_PIXELFORMAT_UYVY:
        return DSPF_UYVY;       /* Packed mode: U0+Y0+V0+Y1 (1 plane) */
    case SDL_PIXELFORMAT_YVYU:
        return DSPF_UNKNOWN;    /* Packed mode: Y0+V0+Y1+U0 (1 plane) */
#if (DFB_VERSION_ATLEAST(1,2,0))
    case SDL_PIXELFORMAT_RGB444:
        return DSPF_RGB444;
#endif
    case SDL_PIXELFORMAT_BGR24:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_BGR888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_RGBA8888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_ABGR8888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_BGRA8888:
        return DSPF_UNKNOWN;
    case SDL_PIXELFORMAT_ARGB2101010:
        return DSPF_UNKNOWN;
    default:
        return DSPF_UNKNOWN;
    }
}

static int
DirectFB_ActivateRenderer(SDL_Renderer * renderer)
{
    SDL_DFB_RENDERERDATA(renderer);
    SDL_Window *window = renderer->window;
    SDL_DFB_WINDOWDATA(window);

    if (renddata->size_changed || windata->wm_needs_redraw) {
//        DirectFB_AdjustWindowSurface(window);
    }
    return 0;
}

static int
DirectFB_DisplayModeChanged(SDL_Renderer * renderer)
{
    SDL_DFB_RENDERERDATA(renderer);

    renddata->size_changed = SDL_TRUE;
    return 0;
}

static int
DirectFB_AcquireVidLayer(SDL_Renderer * renderer, SDL_Texture * texture)
{
    SDL_DFB_RENDERERDATA(renderer);
    SDL_Window *window = renderer->window;
    SDL_VideoDisplay *display = window->display;
    SDL_DFB_DEVICEDATA(display->device);
    DFB_DisplayData *dispdata = (DFB_DisplayData *) display->driverdata;
    DirectFB_TextureData *data = texture->driverdata;
    DFBDisplayLayerConfig layconf;
    DFBResult ret;

    if (renddata->isyuvdirect && (dispdata->vidID >= 0)
        && (!dispdata->vidIDinuse)
        && SDL_ISPIXELFORMAT_FOURCC(data->format)) {
        layconf.flags =
            DLCONF_WIDTH | DLCONF_HEIGHT | DLCONF_PIXELFORMAT |
            DLCONF_SURFACE_CAPS;
        layconf.width = texture->w;
        layconf.height = texture->h;
        layconf.pixelformat = SDLToDFBPixelFormat(data->format);
        layconf.surface_caps = DSCAPS_VIDEOONLY | DSCAPS_DOUBLE;

        SDL_DFB_CHECKERR(devdata->dfb->GetDisplayLayer(devdata->dfb,
                                                       dispdata->vidID,
                                                       &dispdata->vidlayer));
        SDL_DFB_CHECKERR(dispdata->
                         vidlayer->SetCooperativeLevel(dispdata->vidlayer,
                                                       DLSCL_EXCLUSIVE));

        if (devdata->use_yuv_underlays) {
            ret = SDL_DFB_CHECK(dispdata->vidlayer->SetLevel(dispdata->vidlayer, -1));
            if (ret != DFB_OK)
                SDL_DFB_DEBUG("Underlay Setlevel not supported\n");
        }
        SDL_DFB_CHECKERR(dispdata->
                         vidlayer->SetConfiguration(dispdata->vidlayer,
                                                    &layconf));
        SDL_DFB_CHECKERR(dispdata->
                         vidlayer->GetSurface(dispdata->vidlayer,
                                              &data->surface));
        dispdata->vidIDinuse = 1;
        data->display = display;
        return 0;
    }
    return 1;
  error:
    if (dispdata->vidlayer) {
        SDL_DFB_RELEASE(data->surface);
        SDL_DFB_CHECKERR(dispdata->
                         vidlayer->SetCooperativeLevel(dispdata->vidlayer,
                                                       DLSCL_ADMINISTRATIVE));
        SDL_DFB_RELEASE(dispdata->vidlayer);
    }
    return 1;
}

static int
DirectFB_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    SDL_Window *window = renderer->window;
    SDL_VideoDisplay *display = window->display;
    SDL_DFB_DEVICEDATA(display->device);
    DirectFB_TextureData *data;
    DFBSurfaceDescription dsc;
    DFBSurfacePixelFormat pixelformat;

    SDL_DFB_CALLOC(data, 1, sizeof(*data));
    texture->driverdata = data;

    /* find the right pixelformat */
    pixelformat = SDLToDFBPixelFormat(texture->format);
    if (pixelformat == DSPF_UNKNOWN) {
        SDL_SetError("Unknown pixel format %d\n", data->format);
        goto error;
    }

    data->format = texture->format;
    data->pitch = texture->w * DFB_BYTES_PER_PIXEL(pixelformat);

    if (DirectFB_AcquireVidLayer(renderer, texture) != 0) {
        /* fill surface description */
        dsc.flags =
            DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT | DSDESC_CAPS;
        dsc.width = texture->w;
        dsc.height = texture->h;
        /* <1.2 Never use DSCAPS_VIDEOONLY here. It kills performance
         * No DSCAPS_SYSTEMONLY either - let dfb decide
         * 1.2: DSCAPS_SYSTEMONLY boosts performance by factor ~8
         * Depends on other settings as well. Let dfb decide.
         */
        dsc.caps = DSCAPS_PREMULTIPLIED;
#if 0
        if (texture->access == SDL_TEXTUREACCESS_STREAMING)
            dsc.caps |= DSCAPS_SYSTEMONLY;
        else
            dsc.caps |= DSCAPS_VIDEOONLY;
#endif

        dsc.pixelformat = pixelformat;
        data->pixels = NULL;

        /* Create the surface */
        SDL_DFB_CHECKERR(devdata->dfb->CreateSurface(devdata->dfb, &dsc,
                                                     &data->surface));
    }
#if (DFB_VERSION_ATLEAST(1,2,0))
    data->render_options = DSRO_NONE;
#endif

    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        /* 3 plane YUVs return 1 bpp, but we need more space for other planes */
        if(texture->format == SDL_PIXELFORMAT_YV12 ||
           texture->format == SDL_PIXELFORMAT_IYUV) {
            SDL_DFB_CALLOC(data->pixels, 1, (texture->h * data->pitch * 3 + texture->h * data->pitch * 3 % 2) / 2);
        } else {
            SDL_DFB_CALLOC(data->pixels, 1, texture->h * data->pitch);
        }
    }

    return 0;

  error:
    SDL_DFB_RELEASE(data->palette);
    SDL_DFB_RELEASE(data->surface);
    SDL_DFB_FREE(texture->driverdata);
    return -1;
}

static int
DirectFB_QueryTexturePixels(SDL_Renderer * renderer,
                            SDL_Texture * texture, void **pixels, int *pitch)
{
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;

    if (texturedata->display) {
        return -1;
    } else {
        *pixels = texturedata->pixels;
        *pitch = texturedata->pitch;
    }
    return 0;
}

static int
DirectFB_SetTextureScaleMode(SDL_Renderer * renderer, SDL_Texture * texture)
{
#if (DFB_VERSION_ATLEAST(1,2,0))

    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;

    switch (texture->scaleMode) {
    case SDL_SCALEMODE_NONE:
    case SDL_SCALEMODE_FAST:
        data->render_options = DSRO_NONE;
        break;
    case SDL_SCALEMODE_SLOW:
        data->render_options = DSRO_SMOOTH_UPSCALE | DSRO_SMOOTH_DOWNSCALE;
        break;
    case SDL_SCALEMODE_BEST:
        data->render_options =
            DSRO_SMOOTH_UPSCALE | DSRO_SMOOTH_DOWNSCALE | DSRO_ANTIALIAS;
        break;
    default:
        SDL_Unsupported();
        data->render_options = DSRO_NONE;
        texture->scaleMode = SDL_SCALEMODE_NONE;
        return -1;
    }
#endif
    return 0;
}

static int
DirectFB_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                       const SDL_Rect * rect, const void *pixels, int pitch)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;
    Uint8 *dpixels;
    int dpitch;
    Uint8 *src, *dst;
    int row;
    size_t length;
    int bpp = DFB_BYTES_PER_PIXEL(SDLToDFBPixelFormat(texture->format));
    // FIXME: SDL_BYTESPERPIXEL(texture->format) broken for yuv yv12 3 planes

    SDL_DFB_CHECKERR(data->surface->Lock(data->surface,
                                         DSLF_WRITE | DSLF_READ,
                                         ((void **) &dpixels), &dpitch));
    src = (Uint8 *) pixels;
    dst = (Uint8 *) dpixels + rect->y * dpitch + rect->x * bpp;
    length = rect->w * bpp;
    for (row = 0; row < rect->h; ++row) {
        SDL_memcpy(dst, src, length);
        src += pitch;
        dst += dpitch;
    }
    /* copy other planes for 3 plane formats */
    if (texture->format == SDL_PIXELFORMAT_YV12 ||
        texture->format == SDL_PIXELFORMAT_IYUV) {
        src = (Uint8 *) pixels + texture->h * pitch;
        dst = (Uint8 *) dpixels + texture->h * dpitch + rect->y * dpitch / 4 + rect->x * bpp / 2;
        for (row = 0; row < rect->h / 2; ++row) {
            SDL_memcpy(dst, src, length / 2);
            src += pitch / 2;
            dst += dpitch / 2;
        }
        src = (Uint8 *) pixels + texture->h * pitch + texture->h * pitch / 4;
        dst = (Uint8 *) dpixels + texture->h * dpitch + texture->h * dpitch / 4 + rect->y * dpitch / 4 + rect->x * bpp / 2;
        for (row = 0; row < rect->h / 2; ++row) {
            SDL_memcpy(dst, src, length / 2);
            src += pitch / 2;
            dst += dpitch / 2;
        }
    }
    SDL_DFB_CHECKERR(data->surface->Unlock(data->surface));
    return 0;
  error:
    return 1;

}

static int
DirectFB_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                     const SDL_Rect * rect, int markDirty,
                     void **pixels, int *pitch)
{
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;

    if (markDirty) {
        SDL_AddDirtyRect(&texturedata->dirty, rect);
    }

    if (texturedata->display) {
        void *fdata;
        int fpitch;

        SDL_DFB_CHECKERR(texturedata->surface->Lock(texturedata->surface,
                                                    DSLF_WRITE | DSLF_READ,
                                                    &fdata, &fpitch));
        *pitch = fpitch;
        *pixels = fdata;
    } else {
        *pixels =
            (void *) ((Uint8 *) texturedata->pixels +
                      rect->y * texturedata->pitch +
                      rect->x * DFB_BYTES_PER_PIXEL(SDLToDFBPixelFormat(texture->format)));
        *pitch = texturedata->pitch;
    }
    return 0;

  error:
    return -1;
}

static void
DirectFB_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;

    if (texturedata->display) {
        SDL_DFB_CHECK(texturedata->surface->Unlock(texturedata->surface));
        texturedata->pixels = NULL;
    }
}

static void
DirectFB_DirtyTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                      int numrects, const SDL_Rect * rects)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;
    int i;

    for (i = 0; i < numrects; ++i) {
        SDL_AddDirtyRect(&data->dirty, &rects[i]);
    }
}

static int
PrepareDraw(SDL_Renderer * renderer)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    SDL_DFB_WINDOWSURFACE(data->window);

    Uint8 r, g, b, a;

    r = renderer->r;
    g = renderer->g;
    b = renderer->b;
    a = renderer->a;

    SetBlendMode(data, renderer->blendMode, NULL);
    SDL_DFB_CHECKERR(destsurf->SetDrawingFlags(destsurf, data->drawFlags));

    switch (renderer->blendMode) {
    case SDL_BLENDMODE_NONE:
    case SDL_BLENDMODE_BLEND:
        break;
    case SDL_BLENDMODE_ADD:
        r = ((int) r * (int) a) / 255;
        g = ((int) g * (int) a) / 255;
        b = ((int) b * (int) a) / 255;
        a = 255;
        break;
    }

    SDL_DFB_CHECKERR(destsurf->SetColor(destsurf, r, g, b, a));
    return 0;
  error:
    return -1;
}

static int DirectFB_RenderDrawPoints(SDL_Renderer * renderer,
                                const SDL_Point * points, int count)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    SDL_DFB_WINDOWSURFACE(data->window);
    int i;

    PrepareDraw(renderer);
    for (i=0; i < count; i++)
    	SDL_DFB_CHECKERR(destsurf->DrawLine(destsurf, points[i].x, points[i].y, points[i].x, points[i].y));
    return 0;
  error:
    return -1;
}

static int DirectFB_RenderDrawLines(SDL_Renderer * renderer,
                               const SDL_Point * points, int count)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    SDL_DFB_WINDOWSURFACE(data->window);
    int i;

    PrepareDraw(renderer);
    /* Use antialiasing when available */
#if (DFB_VERSION_ATLEAST(1,2,0))
    SDL_DFB_CHECKERR(destsurf->SetRenderOptions(destsurf, DSRO_ANTIALIAS));
#endif

    for (i=0; i < count - 1; i++)
    	SDL_DFB_CHECKERR(destsurf->DrawLine(destsurf, points[i].x, points[i].y, points[i+1].x, points[i+1].y));

    return 0;
  error:
    return -1;
}

static int
DirectFB_RenderFillRects(SDL_Renderer * renderer, const SDL_Rect ** rects, int count)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    SDL_DFB_WINDOWSURFACE(data->window);
    int i;

    PrepareDraw(renderer);

    for (i=0; i<count; i++)
    	SDL_DFB_CHECKERR(destsurf->FillRectangle(destsurf, rects[i]->x, rects[i]->y,
    			rects[i]->w, rects[i]->h));

    return 0;
  error:
    return -1;
}

static int
DirectFB_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                    const SDL_Rect * srcrect, const SDL_Rect * dstrect)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    SDL_DFB_WINDOWSURFACE(data->window);
    DirectFB_TextureData *texturedata =
        (DirectFB_TextureData *) texture->driverdata;
    Uint8 alpha = 0xFF;

    if (texturedata->display) {
        int px, py;
        SDL_Window *window = renderer->window;
        SDL_DFB_WINDOWDATA(window);
        SDL_VideoDisplay *display = texturedata->display;
        DFB_DisplayData *dispdata = (DFB_DisplayData *) display->driverdata;

        SDL_DFB_CHECKERR(dispdata->
                         vidlayer->SetSourceRectangle(dispdata->vidlayer,
                                                      srcrect->x, srcrect->y,
                                                      srcrect->w,
                                                      srcrect->h));
        SDL_DFB_CHECK(windata->window->GetPosition(windata->window, &px, &py));
        px += windata->client.x;
        py += windata->client.y;
        SDL_DFB_CHECKERR(dispdata->
                         vidlayer->SetScreenRectangle(dispdata->vidlayer,
                                                      px + dstrect->x,
                                                      py + dstrect->y,
                                                      dstrect->w,
                                                      dstrect->h));
    } else {
        DFBRectangle sr, dr;
        DFBSurfaceBlittingFlags flags = 0;

        if (texturedata->dirty.list) {
            SDL_DirtyRect *dirty;
            void *pixels;
            int bpp = DFB_BYTES_PER_PIXEL(SDLToDFBPixelFormat(texture->format));
            int pitch = texturedata->pitch;

            for (dirty = texturedata->dirty.list; dirty; dirty = dirty->next) {
                SDL_Rect *rect = &dirty->rect;
                pixels =
                    (void *) ((Uint8 *) texturedata->pixels +
                              rect->y * pitch + rect->x * bpp);
                DirectFB_UpdateTexture(renderer, texture, rect,
                                       texturedata->pixels,
                                       texturedata->pitch);
            }
            SDL_ClearDirtyRects(&texturedata->dirty);
        }

        SDLtoDFBRect(srcrect, &sr);
        SDLtoDFBRect(dstrect, &dr);

        SDL_DFB_CHECKERR(destsurf->
                         SetColor(destsurf, 0xFF, 0xFF, 0xFF, 0xFF));
        if (texture->
            modMode & (SDL_TEXTUREMODULATE_COLOR | SDL_TEXTUREMODULATE_ALPHA))
        {
            if (texture->modMode & SDL_TEXTUREMODULATE_ALPHA) {
                alpha = texture->a;
                SDL_DFB_CHECKERR(destsurf->SetColor(destsurf, 0xFF, 0xFF,
                                                    0xFF, alpha));
            }
            if (texture->modMode & SDL_TEXTUREMODULATE_COLOR) {

                SDL_DFB_CHECKERR(destsurf->SetColor(destsurf,
                                                    texture->r,
                                                    texture->g,
                                                    texture->b, alpha));
                flags |= DSBLIT_COLORIZE;
            }
            if (alpha < 0xFF)
                flags |= DSBLIT_SRC_PREMULTCOLOR;
        } else
            SDL_DFB_CHECKERR(destsurf->SetColor(destsurf, 0xFF, 0xFF,
                                                0xFF, 0xFF));

        SetBlendMode(data, texture->blendMode, texturedata);

        SDL_DFB_CHECKERR(destsurf->SetBlittingFlags(destsurf,
                                                    data->blitFlags | flags));

#if (DFB_VERSION_ATLEAST(1,2,0))
        SDL_DFB_CHECKERR(destsurf->SetRenderOptions(destsurf,
                                                    texturedata->
                                                    render_options));
#endif

        if (srcrect->w == dstrect->w && srcrect->h == dstrect->h) {
            SDL_DFB_CHECKERR(destsurf->Blit(destsurf,
                                            texturedata->surface,
                                            &sr, dr.x, dr.y));
        } else {
            SDL_DFB_CHECKERR(destsurf->StretchBlit(destsurf,
                                                   texturedata->surface,
                                                   &sr, &dr));
        }
    }
    return 0;
  error:
    return -1;
}

static void
DirectFB_RenderPresent(SDL_Renderer * renderer)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;
    SDL_Window *window = renderer->window;
    SDL_DFB_WINDOWDATA(window);

    DFBRectangle sr;

    sr.x = 0;
    sr.y = 0;
    sr.w = window->w;
    sr.h = window->h;

    /* Send the data to the display */
    SDL_DFB_CHECK(windata->window_surface->Flip(windata->window_surface, NULL,
                                                data->flipflags));
}

static void
DirectFB_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    DirectFB_TextureData *data = (DirectFB_TextureData *) texture->driverdata;

    if (!data) {
        return;
    }
    SDL_DFB_RELEASE(data->palette);
    SDL_DFB_RELEASE(data->surface);
    if (data->display) {
        DFB_DisplayData *dispdata =
            (DFB_DisplayData *) data->display->driverdata;
        dispdata->vidIDinuse = 0;
        /* FIXME: Shouldn't we reset the cooperative level */
        SDL_DFB_CHECK(dispdata->vidlayer->SetCooperativeLevel(dispdata->vidlayer,
                                                DLSCL_ADMINISTRATIVE));
        SDL_DFB_RELEASE(dispdata->vidlayer);
    }
    SDL_FreeDirtyRects(&data->dirty);
    SDL_DFB_FREE(data->pixels);
    SDL_free(data);
    texture->driverdata = NULL;
}

static void
DirectFB_DestroyRenderer(SDL_Renderer * renderer)
{
    DirectFB_RenderData *data = (DirectFB_RenderData *) renderer->driverdata;

    if (data) {
        SDL_free(data);
    }
    SDL_free(renderer);
}

/* vi: set ts=4 sw=4 expandtab: */
