/* Picasso - a vector graphics library
 * 
 * Copyright (C) 2013 Zhang Ji Peng
 * Contact: onecoolx@gmail.com
 */

#include <stdio.h>
#include "common.h"
#include "device.h"
#include "graphic_path.h"
#include "geometry.h"
#include "convert.h"

#include "picasso.h"
#include "picasso_global.h"
#include "picasso_painter.h"
#include "picasso_font.h"
#include "picasso_objects.h"

namespace picasso {

font_engine::font_engine(unsigned int max_fonts)
    : m_fonts(pod_allocator<font_adapter*>::allocate(max_fonts))
    , m_current(0)
    , m_max_fonts(max_fonts)
    , m_num_fonts(0)
    , m_signature(0)
    , m_stamp_change(false)
    , m_antialias(false)
{
    memset(m_fonts, 0, sizeof(font_adapter*) * max_fonts);
    m_signature = (char*)mem_calloc(1, MAX_FONT_NAME_LENGTH + 128);
}

font_engine::~font_engine()
{
    if (m_signature)
        mem_free(m_signature);

    for (unsigned int i = 0; i < m_num_fonts; ++i) 
        delete m_fonts[i];
    
    pod_allocator<font_adapter*>::deallocate(m_fonts, m_max_fonts);
}

void font_engine::set_antialias(bool b)
{
    if (m_antialias != b) {
        m_antialias = b;
        m_stamp_change = true;
    }
}

void font_engine::set_transform(const trans_affine& mtx) 
{
    if (m_affine != mtx){
        m_affine = mtx;
        m_stamp_change = true;
    }
}

int font_engine::find_font(const char* font_signature)
{
    for (unsigned int i = 0; i < m_num_fonts; i++) {
        if(strcmp(m_fonts[i]->signature(), font_signature) == 0) 
            return (int)i;
    }
    return -1;
}

bool font_engine::create_font(const font_desc& desc)
{
    if (!font_adapter::create_signature(desc, m_affine, m_antialias, m_signature))
        return false;

    if (m_current)
        m_current->deactive();

    int idx = find_font(m_signature);
    if (idx >= 0) {
        m_current = m_fonts[idx];
    } else {
        if (m_num_fonts >= m_max_fonts) {
            delete m_fonts[0];
            memcpy (m_fonts, m_fonts + 1, (m_max_fonts - 1) * sizeof(font_adapter*)); 
            m_num_fonts = m_max_fonts - 1;
        }

        m_fonts[m_num_fonts] = new font_adapter(desc, m_signature, m_affine, m_antialias);
        m_current = m_fonts[m_num_fonts];
        m_num_fonts++;
    }

    m_current->active();
    m_stamp_change = false;
    return true;
}

bool font_engine::initialize(void)
{
    return platform_font_init();
}

void font_engine::shutdown(void)
{
    platform_font_shutdown();
}

// font adapter
bool font_adapter::create_signature(const font_desc& desc, const trans_affine& mtx, bool anti, char* recv_sig)
{
    if (!recv_sig)
        return false; //out of memory.

    recv_sig[0] = 0;
    
    sprintf(recv_sig,
            "%s,%d,%d,%d,%d,%d,%d,%d-",
            desc.name(),
            desc.charset(),
            (int)desc.height(),
            (int)desc.weight(),
            (int)desc.italic(),
            (int)desc.hint(),
            (int)desc.flip_y(),
            (int)anti);

    char mbuf[64] = {0};
    sprintf(mbuf,
            "%08X%08X%08X%08X%08X%08X",
            fxmath::dbl_to_fixed(SCALAR_TO_DBL(mtx.sx())), 
            fxmath::dbl_to_fixed(SCALAR_TO_DBL(mtx.sy())), 
            fxmath::dbl_to_fixed(SCALAR_TO_DBL(mtx.shx())), 
            fxmath::dbl_to_fixed(SCALAR_TO_DBL(mtx.shy())), 
            fxmath::dbl_to_fixed(SCALAR_TO_DBL(mtx.tx())), 
            fxmath::dbl_to_fixed(SCALAR_TO_DBL(mtx.ty())));

    strcat(recv_sig, mbuf);
    return true;
}

void font_adapter::active(void)
{
    m_impl->active(); 
    m_prev_glyph = m_last_glyph = 0;
}

void font_adapter::deactive(void)
{
    m_prev_glyph = m_last_glyph = 0;
    m_impl->deactive();
}

void font_adapter::add_kerning(scalar* x, scalar* y)
{
    if (m_prev_glyph && m_last_glyph)
        m_impl->add_kerning(m_prev_glyph->index, m_last_glyph->index, x, y);
}

const glyph* font_adapter::get_glyph(unsigned int code)
{
    const glyph* gl = m_cache->find_glyph(code);
    if (gl) {
        m_prev_glyph = m_last_glyph;
        m_last_glyph = gl;
        return gl;
    } else {
        if (m_impl->prepare_glyph(code)) {
            m_prev_glyph = m_last_glyph;
            gl = m_cache->cache_glyph(code,
                                 m_impl->glyph_index(),
                                 m_impl->data_size(),
                                 m_impl->data_type(),
                                 m_impl->bounds(),
                                 m_impl->height(),
                                 m_impl->advance_x(),
                                 m_impl->advance_y());  
            m_impl->write_glyph_to(gl->data);
            m_last_glyph = gl;
            return gl;
        }
    }
    return 0;
}

bool font_adapter::generate_raster(const glyph* g, scalar x, scalar y)
{
    if (g) {
        if (g->type == glyph_type_mono) {
            m_mono_storage.serialize_from(g->data, g->data_size, x, y);
            return true;
        } else {
            unsigned int count = 0;
            unsigned int offset = sizeof(unsigned int);
            memcpy(&count, g->data, offset);
            m_path_adaptor.serialize_from(count, g->data+offset, g->data_size-offset);
            m_path_adaptor.translate(x, y);
        }
        return true;
    } else
        return false;
}

ps_font* _default_font(void)
{
    static ps_font* font = 0;
    if (!font) {
        ps_font* f = (ps_font*)mem_malloc(sizeof(ps_font));
        if (f) {
            f->refcount = 1;
            new ((void*)&(f->desc)) font_desc("Arial");
            f->desc.set_charset(CHARSET_ANSI);
            f->desc.set_height(12);
            f->desc.set_weight(400);
            f->desc.set_italic(false);
            f->desc.set_hint(true);
            f->desc.set_flip_y(true);
            font = f;
        }
    }
    return font;
}

}

static bool inline create_device_font(ps_context* ctx)
{
	ctx->fonts->set_transform(ctx->text_matrix);
	ctx->fonts->set_antialias(ctx->font_antialias ? true : false);

    // same with current font.
	if (!ctx->fonts->stamp_change() && ctx->fonts->current_font()
        && (ctx->fonts->current_font()->desc() == ctx->state->font->desc))
		return true;

	if (ctx->fonts->create_font(ctx->state->font->desc)) {
		return true;
    } else
		return false;
}

static inline void _add_glyph_to_path(ps_context* ctx, picasso::graphic_path& path)	
{
    picasso::conv_curve curve(ctx->fonts->current_font()->path_adaptor());

	scalar x = 0;
	scalar y = 0;

	while (true) {
		unsigned int cmd = curve.vertex(&x, &y);
        if (picasso::is_stop(cmd)) {
            path.add_vertex(x, y, picasso::path_cmd_end_poly|picasso::path_flags_close);
            break;
        } else 
            path.add_vertex(x, y, cmd);
	}
}


#ifdef __cplusplus
extern "C" {
#endif

ps_font* PICAPI ps_font_create_copy(const ps_font* font)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return 0;
	}

	if (!font) {
		global_status = STATUS_INVALID_ARGUMENT;
		return 0;
	}

	ps_font* f = (ps_font*)mem_malloc(sizeof(ps_font));
	if (f) {
		f->refcount = 1;
        new ((void*)&(f->desc)) picasso::font_desc(font->desc.name());
        f->desc.set_charset(font->desc.charset());
        f->desc.set_height(font->desc.height());
        f->desc.set_weight(font->desc.weight());
        f->desc.set_italic(font->desc.italic());
        f->desc.set_hint(font->desc.hint());
        f->desc.set_flip_y(font->desc.flip_y());
		global_status = STATUS_SUCCEED;
		return f;
	} else {
        global_status = STATUS_OUT_OF_MEMORY;
		return 0;
	}
}

ps_font* PICAPI ps_font_create(const char* name, ps_charset c, double s, int w, ps_bool i)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return 0;
	}

	if (!name) {
		global_status = STATUS_INVALID_ARGUMENT;
		return 0;
	}

	ps_font* f = (ps_font*)mem_malloc(sizeof(ps_font));
	if (f) {
		f->refcount = 1;
        new ((void*)&(f->desc)) picasso::font_desc(name);
        f->desc.set_charset(c);
        f->desc.set_height(DBL_TO_SCALAR(s));
        f->desc.set_weight(DBL_TO_SCALAR(w));
        f->desc.set_italic(i ? true: false);
        f->desc.set_hint(true);
        f->desc.set_flip_y(true);
		global_status = STATUS_SUCCEED;
		return f;
	} else {
        global_status = STATUS_OUT_OF_MEMORY;
		return 0;
	}
}

ps_font* PICAPI ps_font_ref(ps_font* f)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return 0;
	}

	if (!f) {
		global_status = STATUS_INVALID_ARGUMENT;
		return 0;
	}

	f->refcount++;
	global_status = STATUS_SUCCEED;
	return f;
}

void PICAPI ps_font_unref(ps_font* f)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!f) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	f->refcount--;
	if (f->refcount <= 0) {
		mem_free(f);
	}
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_font_set_size(ps_font* f, double s)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!f || s < 0.0) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}
	
	f->desc.set_height(DBL_TO_SCALAR(s));
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_font_set_weight(ps_font* f, int w)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!f || w < 100 || w > 900) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	f->desc.set_weight(DBL_TO_SCALAR(w));
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_font_set_italic(ps_font* f, ps_bool it)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!f) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	f->desc.set_italic(it ? true : false);
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_font_set_hint(ps_font* f, ps_bool h)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!f) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	f->desc.set_hint(h ? true : false);
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_font_set_flip(ps_font* f, ps_bool l)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!f) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}
	
    //Note: FIXME:this will change next time.
    f->desc.set_flip_y(l ? false : true);
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_font_set_charset(ps_font* f, ps_charset c)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!f) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	f->desc.set_charset(c);
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_text_out_length(ps_context* ctx, double x, double y, const char* text, unsigned int len)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !text || !len) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	scalar gx = DBL_TO_SCALAR(x);
	scalar gy = DBL_TO_SCALAR(y);

	if (create_device_font(ctx)) {
		gy += ctx->fonts->current_font()->ascent();

		const char* p = text;
		while (*p && len) {
            register char c = *p;
			const picasso::glyph* glyph = ctx->fonts->current_font()->get_glyph(c);
			if (glyph) {
                if (ctx->font_kerning)
                    ctx->fonts->current_font()->add_kerning(&gx, &gy);
                if (ctx->fonts->current_font()->generate_raster(glyph, gx, gy)) 
                   ctx->canvas->p->render_glyph(ctx->state, ctx->raster, ctx->fonts->current_font(), glyph->type);

				gx += glyph->advance_x;
				gy += glyph->advance_y;
			}
			len--;
			p++;
		}
		ctx->canvas->p->render_glyphs_raster(ctx->state, ctx->raster, ctx->font_render_type);
	}
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_wide_text_out_length(ps_context* ctx, double x, double y, const ps_uchar16* text, unsigned int len)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !text || !len) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	scalar gx = DBL_TO_SCALAR(x);
	scalar gy = DBL_TO_SCALAR(y);

	if (create_device_font(ctx)) {
		gy += ctx->fonts->current_font()->ascent();

		const ps_uchar16* p = text;
		while (*p && len) {
            register ps_uchar16 c = *p;
			const picasso::glyph* glyph = ctx->fonts->current_font()->get_glyph(c);
			if (glyph) {
                if (ctx->font_kerning)
                    ctx->fonts->current_font()->add_kerning(&gx, &gy);
                if (ctx->fonts->current_font()->generate_raster(glyph, gx, gy)) 
                   ctx->canvas->p->render_glyph(ctx->state, ctx->raster, ctx->fonts->current_font(), glyph->type);

				gx += glyph->advance_x;
				gy += glyph->advance_y;
			}
			len--;
			p++;
		}
		ctx->canvas->p->render_glyphs_raster(ctx->state, ctx->raster, ctx->font_render_type);
	}
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_draw_text(ps_context* ctx, const ps_rect* area, const void* text, unsigned int len,
																ps_draw_text_type type, ps_text_align align)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !area || !text || !len) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	scalar x = DBL_TO_SCALAR(area->x);
	scalar y = DBL_TO_SCALAR(area->y);

	picasso::graphic_path text_path;

    ps_bool text_antialias = ctx->font_antialias;
    ctx->font_antialias = True;
	if (create_device_font(ctx)) {

		// align layout
		scalar w = 0, h = 0;
		const picasso::glyph* glyph_test = 0;

		if (ctx->state->font->desc.charset() == CHARSET_ANSI) {
			const char* p = (const char*)text;
			glyph_test = ctx->fonts->current_font()->get_glyph(*p);
		} else {
			const ps_uchar16* p = (const ps_uchar16*)text;
			glyph_test = ctx->fonts->current_font()->get_glyph(*p);
		}

		if (glyph_test) {
			w = glyph_test->advance_x;
			h = glyph_test->height; //Note: advance_y always 0.
		}

		w *= len; //FIXME: estimate!

		if (align & TEXT_ALIGN_LEFT)
			x = DBL_TO_SCALAR(area->x);
		else if (align & TEXT_ALIGN_RIGHT)
			x = DBL_TO_SCALAR(area->x + (area->w - w));
		else
			x = DBL_TO_SCALAR(area->x + (area->w - w)/2);

		if (align & TEXT_ALIGN_TOP) {
			y = DBL_TO_SCALAR(area->y);
			y += ctx->fonts->current_font()->ascent();
		} else if (align & TEXT_ALIGN_BOTTOM) {
			y = DBL_TO_SCALAR(area->y + (area->h - SCALAR_TO_DBL(h)));
			y -= ctx->fonts->current_font()->descent();
		} else {
			y = DBL_TO_SCALAR(area->y + (area->h - SCALAR_TO_DBL(h))/2);
			y += (ctx->fonts->current_font()->ascent() - ctx->fonts->current_font()->descent())/2;
		}

		// draw the text
		if (ctx->state->font->desc.charset() == CHARSET_ANSI) {
			const char* p = (const char*)text;
			while (*p && len) {
                register char c = *p;
				const picasso::glyph* glyph = ctx->fonts->current_font()->get_glyph(c);
				if (glyph) {
                    if (ctx->font_kerning)
                        ctx->fonts->current_font()->add_kerning(&x, &y);
                    if (ctx->fonts->current_font()->generate_raster(glyph, x, y))
                        _add_glyph_to_path(ctx, text_path);

					x += glyph->advance_x;
					y += glyph->advance_y;
				}
				len--;
				p++;
			}
		} else {
			const ps_uchar16* p = (const ps_uchar16*)text;
			while (*p && len) {
                register ps_uchar16 c = *p;
				const picasso::glyph* glyph = ctx->fonts->current_font()->get_glyph(c);
				if (glyph) {
                    if (ctx->font_kerning)
                        ctx->fonts->current_font()->add_kerning(&x, &y);
                    if (ctx->fonts->current_font()->generate_raster(glyph, x, y))
                        _add_glyph_to_path(ctx, text_path);

					x += glyph->advance_x;
					y += glyph->advance_y;
				}
				len--;
				p++;
			}
		}
	}

	text_path.close_polygon();
    ctx->font_antialias = text_antialias;

	//store the old color
	picasso::rgba bc = ctx->state->brush.color;
	picasso::rgba pc = ctx->state->pen.color;

	ctx->state->brush.color = ctx->state->font_fcolor;
	ctx->state->pen.color = ctx->state->font_scolor;

	switch (type) {
		case DRAW_TEXT_FILL:
			ctx->canvas->p->render_shadow(ctx->state, text_path, true, false);
    		ctx->canvas->p->render_fill(ctx->state, ctx->raster, text_path);
    		ctx->canvas->p->render_blur(ctx->state);
			break;
		case DRAW_TEXT_STROKE:
			ctx->canvas->p->render_shadow(ctx->state, text_path, false, true);
    		ctx->canvas->p->render_stroke(ctx->state, ctx->raster, text_path);
    		ctx->canvas->p->render_blur(ctx->state);
			break;
		case DRAW_TEXT_BOTH:
			ctx->canvas->p->render_shadow(ctx->state, text_path, true, true);
    		ctx->canvas->p->render_paint(ctx->state, ctx->raster, text_path);
    		ctx->canvas->p->render_blur(ctx->state);
			break;
	}

	ctx->state->brush.color = bc;
	ctx->state->pen.color = pc;
	ctx->raster.reset();
	global_status = STATUS_SUCCEED;
}

ps_size PICAPI ps_get_text_extent(ps_context* ctx, const void* text, unsigned int len)
{
	ps_size size = {0 , 0};

	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return size;
	}

	if (!ctx || !text || !len) {
		global_status = STATUS_INVALID_ARGUMENT;
		return size;
	}

	scalar width = 0;

	if (create_device_font(ctx)) {

		if (ctx->state->font->desc.charset() == CHARSET_ANSI) {
			const char* p = (const char*)text;
			while (*p && len) {
                register char c = *p;
				const picasso::glyph* glyph = ctx->fonts->current_font()->get_glyph(c);
				if (glyph) 
					width += glyph->advance_x;
				
				len--;
				p++;
			}
		} else {
			const ps_uchar16* p = (const ps_uchar16*)text;
			while (*p && len) {
                register ps_uchar16 c = *p;
				const picasso::glyph* glyph = ctx->fonts->current_font()->get_glyph(c);
				if (glyph) 
					width += glyph->advance_x;
				
				len--;
				p++;
			}
		}
	}

	size.h = SCALAR_TO_DBL(ctx->fonts->current_font()->height());
	size.w = SCALAR_TO_DBL(width);
	global_status = STATUS_SUCCEED;
	return size;
}

void PICAPI ps_show_glyphs(ps_context* ctx, double x, double y,  ps_glyph* g, unsigned int len)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !g || !len) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	scalar gx = DBL_TO_SCALAR(x);
	scalar gy = DBL_TO_SCALAR(y);

	if (create_device_font(ctx)) {
        gy += ctx->fonts->current_font()->ascent();
		for (unsigned int i = 0; i < len; i++) {
			const picasso::glyph* glyph = (const picasso::glyph*)g[i].glyph;
            if (glyph) {
                if (ctx->font_kerning)
                    ctx->fonts->current_font()->add_kerning(&gx, &gy);
                if (ctx->fonts->current_font()->generate_raster(glyph, gx, gy)) 
                   ctx->canvas->p->render_glyph(ctx->state, ctx->raster, ctx->fonts->current_font(), glyph->type);

                gx += glyph->advance_x;
                gy += glyph->advance_y;
            }
		}
		ctx->canvas->p->render_glyphs_raster(ctx->state, ctx->raster, ctx->font_render_type);
	}
	global_status = STATUS_SUCCEED;
}

ps_bool PICAPI ps_get_path_from_glyph(ps_context* ctx, const ps_glyph* g, ps_path* p)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return False;
	}

	if (!ctx || !g || !p) {
		global_status = STATUS_INVALID_ARGUMENT;
		return False;
	}

	p->path.remove_all(); // clear the path

    scalar x = 0;
    scalar y = 0;

    ps_bool text_antialias = ctx->font_antialias;
    ctx->font_antialias = True;
	if (create_device_font(ctx)) {
		const picasso::glyph* gly = (const picasso::glyph*)g->glyph;
		if (gly) {
            if (gly->type != picasso::glyph_type_outline)
                gly = ctx->fonts->current_font()->get_glyph(gly->code);

            if (gly) { // Must be outline glyph for path.
                y += ctx->fonts->current_font()->ascent();
                if (ctx->fonts->current_font()->generate_raster(gly, x, y))
                    _add_glyph_to_path(ctx, p->path);
            }
		}
	}

	p->path.close_polygon();
    ctx->font_antialias = text_antialias;
	global_status = STATUS_SUCCEED;
	return True;
}

ps_bool PICAPI ps_get_glyph(ps_context* ctx, int ch, ps_glyph* g)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return False;
	}

	if (!ctx || !g) {
		global_status = STATUS_INVALID_ARGUMENT;
		return False;
	}

	if (create_device_font(ctx)) {
		if (ctx->state->font->desc.charset() == CHARSET_ANSI) {
			char c = (char)ch;
			g->glyph = (void*)ctx->fonts->current_font()->get_glyph(c);
		} else {
			ps_uchar16 c = (ps_uchar16)ch;
			g->glyph = (void*)ctx->fonts->current_font()->get_glyph(c);
		}
		global_status = STATUS_SUCCEED;
		return True;
	} else {
		g->glyph = NULL;
		global_status = STATUS_UNKNOWN_ERROR;
		return False;
	}
}

ps_size PICAPI ps_glyph_get_extent(const ps_glyph* g)
{
	ps_size size = {0 , 0};
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return size;
	}

	if (!g) {
		global_status = STATUS_INVALID_ARGUMENT;
		return size;
	}

	if (g->glyph) {
		const picasso::glyph* gp = static_cast<const picasso::glyph*>(g->glyph);
		size.w = SCALAR_TO_DBL(gp->advance_x);
		size.h = SCALAR_TO_DBL(gp->height); //Note: advance_y is 0
	}
	global_status = STATUS_SUCCEED;
	return size;
}

ps_bool PICAPI ps_get_font_info(ps_context* ctx, ps_font_info* info)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return False;
	}

	if (!ctx || !info) {
		global_status = STATUS_INVALID_ARGUMENT;
		return False;
	}

	if (create_device_font(ctx)) {
		info->size = SCALAR_TO_DBL(ctx->fonts->current_font()->height());
		info->ascent = SCALAR_TO_DBL(ctx->fonts->current_font()->ascent());
		info->descent = SCALAR_TO_DBL(ctx->fonts->current_font()->descent());
		info->leading = SCALAR_TO_DBL(ctx->fonts->current_font()->leading());
		info->unitsEM = SCALAR_TO_INT(ctx->fonts->current_font()->units_per_em());
        global_status = STATUS_SUCCEED;
        return True;
	}

    global_status = STATUS_UNKNOWN_ERROR;
    return False;
}

ps_font* PICAPI ps_set_font(ps_context* ctx, const ps_font* f)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return 0;
	}

	if (!ctx || !f) {
		global_status = STATUS_INVALID_ARGUMENT;
		return 0;
	}

	ps_font* old = ctx->state->font;
	ps_font_unref(ctx->state->font);
	ctx->state->font = ps_font_ref(const_cast<ps_font*>(f));
	global_status = STATUS_SUCCEED;
	return old;
}

void PICAPI ps_set_text_render_type(ps_context* ctx, ps_text_type type)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	ctx->font_render_type = type;
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_set_text_kerning(ps_context* ctx, ps_bool k)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}
	
	ctx->font_kerning = k;
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_set_text_antialias(ps_context* ctx, ps_bool a)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	ctx->font_antialias = a;
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_set_text_color(ps_context* ctx, const ps_color* c)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !c) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	ctx->state->font_fcolor =
         picasso::rgba(DBL_TO_SCALAR(c->r), DBL_TO_SCALAR(c->g), DBL_TO_SCALAR(c->b), DBL_TO_SCALAR(c->a));
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_set_text_stroke_color(ps_context* ctx, const ps_color* c)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !c) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	ctx->state->font_scolor =
         picasso::rgba(DBL_TO_SCALAR(c->r), DBL_TO_SCALAR(c->g), DBL_TO_SCALAR(c->b), DBL_TO_SCALAR(c->a));
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_text_transform(ps_context* ctx, const ps_matrix* m)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !m) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	ctx->text_matrix *= m->matrix;
	global_status = STATUS_SUCCEED;
}

void PICAPI ps_set_text_matrix(ps_context* ctx, const ps_matrix* m)
{
	if (!picasso::is_valid_system_device()) {
		global_status = STATUS_DEVICE_ERROR;
		return;
	}

	if (!ctx || !m) {
		global_status = STATUS_INVALID_ARGUMENT;
		return;
	}

	ctx->text_matrix = m->matrix;
	global_status = STATUS_SUCCEED;
}

#ifdef __cplusplus
}
#endif