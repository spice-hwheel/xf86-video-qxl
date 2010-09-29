#include "qxl.h"

struct qxl_surface_t
{
    qxl_screen_t *	qxl;
    
    uint32_t	        id;

    pixman_image_t *	dev_image;
    pixman_image_t *	host_image;

    RegionRec		access_region;

    void *		address;
    void *		end;
    
    qxl_surface_t *	next;
    int			in_use;
    int			Bpp;
    int			ref_count;

    union
    {
	qxl_surface_t *copy_src;
	Pixel	       solid_pixel;
    } u;
};

static qxl_surface_t *all_surfaces;
static qxl_surface_t *free_surfaces;

#if 0
void
qxl_surface_free_all (qxl_screen_t *qxl)
{
    for (i = 0; i < qxl->rom->n_surfaces; ++i)
    {
	qxl_surface_t *surface = &(all_surfaces[i]);
	if (surface->in_use)
	{
	    
	}
    }
}
#endif

void
qxl_surface_flatten_all (qxl_screen_t *qxl)
{
    int i;

#if 0
    for (i = 0; i < qxl->rom->n_surface; ++i)
    {
	qxl_surface_t *surface = &(all_surfaces[i]);

	
    }
#endif

}

void
qxl_surface_init (qxl_screen_t *qxl, int n_surfaces)
{
    int i;

    if (!all_surfaces)
	all_surfaces = calloc (n_surfaces, sizeof (qxl_surface_t));

    for (i = 0; i < n_surfaces; ++i)
    {
	all_surfaces[i].id = i;
	all_surfaces[i].qxl = qxl;
	all_surfaces[i].dev_image = NULL;
	all_surfaces[i].host_image = NULL;
	
	REGION_INIT (NULL, &(all_surfaces[i].access_region), (BoxPtr)NULL, 0);

	if (i) /* surface 0 is the primary surface */
	{
	    all_surfaces[i].next = free_surfaces;
	    free_surfaces = &(all_surfaces[i]);
	    all_surfaces[i].in_use = FALSE;
	}
    }
}

static qxl_surface_t *
surface_new (void)
{
    qxl_surface_t *result = NULL;

    if (free_surfaces)
    {
	result = free_surfaces;
	free_surfaces = free_surfaces->next;
    }

    result->next = NULL;
    result->in_use = TRUE;
    result->ref_count = 1;
    
    return result;
}

static void
surface_free (qxl_surface_t *surface)
{
    surface->next = free_surfaces;
    free_surfaces = surface;
}

qxl_surface_t *
qxl_surface_create_primary (qxl_screen_t	*qxl,
			    struct qxl_mode	*mode)
{
    struct qxl_ram_header *ram_header =
	(void *)((unsigned long)qxl->ram + qxl->rom->ram_header_offset);
    struct qxl_surface_create *create = &(ram_header->create_surface);
    pixman_format_code_t format;
    uint8_t *dev_addr;
    pixman_image_t *dev_image, *host_image;
    qxl_surface_t *surface;

    if (mode->bits == 16)
    {
	format = PIXMAN_x1r5g5b5;
    }
    else if (mode->bits == 32)
    {
	format = PIXMAN_x8r8g8b8;
    }
    else
    {
	xf86DrvMsg (qxl->pScrn->scrnIndex, X_ERROR,
		    "Unknown bit depth %d\n", mode->bits);
	return NULL;
    }
	
    create->width = mode->x_res;
    create->height = mode->y_res;
    create->stride = - mode->stride;
    create->depth = mode->bits;
    create->position = 0; /* What is this? The Windows driver doesn't use it */
    create->flags = 0;
    create->type = QXL_SURF_TYPE_PRIMARY;
    create->mem = physical_address (qxl, qxl->ram, qxl->main_mem_slot);

    outb (qxl->io_base + QXL_IO_CREATE_PRIMARY, 0);

    dev_addr = (uint8_t *)qxl->ram + mode->stride * (mode->y_res - 1);

    dev_image = pixman_image_create_bits (format, mode->x_res, mode->y_res,
					  (uint32_t *)dev_addr, -mode->stride);

    host_image = pixman_image_create_bits (format, 
					   qxl->virtual_x, qxl->virtual_y,
					   qxl->fb, qxl->stride);

    surface = malloc (sizeof *surface);
    surface->id = 0;
    surface->dev_image = dev_image;
    surface->host_image = host_image;
    surface->qxl = qxl;
    surface->Bpp = PIXMAN_FORMAT_BPP (format) / 8;
    
    REGION_INIT (NULL, &(surface->access_region), (BoxPtr)NULL, 0);
    
    return surface;
}

static struct qxl_surface_cmd *
make_surface_cmd (qxl_screen_t *qxl, uint32_t id, qxl_surface_cmd_type type)
{
    struct qxl_surface_cmd *cmd;

    cmd = qxl_allocnf (qxl, sizeof *cmd);

    cmd->release_info.id = pointer_to_u64 (cmd) | 2;
    cmd->type = type;
    cmd->flags = 0;
    cmd->surface_id = id;
    
    return cmd;
}

static void
push_surface_cmd (qxl_screen_t *qxl, struct qxl_surface_cmd *cmd)
{
    struct qxl_command command;

    if (!in_vga_mode (qxl))
    {
	command.type = QXL_CMD_SURFACE;
	command.data = physical_address (qxl, cmd, qxl->main_mem_slot);
	
	qxl_ring_push (qxl->command_ring, &command);
    }
}

qxl_surface_t *
qxl_surface_create (qxl_screen_t *qxl,
		    int		  width,
		    int		  height,
		    int	          bpp)
{
    qxl_surface_t *surface;
    struct qxl_surface_cmd *cmd;
    qxl_bitmap_format format;
    pixman_format_code_t pformat;
    int stride;
    uint32_t *dev_addr;
    static int count;
    int n_attempts = 0;

    if (++count < 200)
      return NULL;

#if 0
    ErrorF ("   qxl_surface: attempting to allocate %d x %d @ %d\n", width, height, bpp);
#endif
    
    if ((bpp & 3) != 0)
    {
	ErrorF ("   Bad bpp: %d (%d)\n", bpp, bpp & 7);
	return NULL;
    }

    if (bpp == 8)
      {
	static int warned = 10;
	if (warned > 0)
	{
	    warned--;
	    ErrorF ("bpp == 8 triggers bugs in spice apparently\n");
	}
	
	return NULL;
      }
    
    if (bpp != 8 && bpp != 16 && bpp != 32 && bpp != 24)
    {
	ErrorF ("   Unknown bpp\n");
	return NULL;
    }

retry:
    surface = surface_new();
    if (!surface)
    {
	if (!qxl_handle_oom (qxl))
	{
	    ErrorF ("  Out of surfaces\n");
	    return NULL;
	}
	else
	    goto retry;
    }

#if 0
    ErrorF ("    Surface allocated: %u\n", surface->id);
    ErrorF ("Allocated %d\n", surface->id);
#endif
    
    if (width == 0 || height == 0)
    {
	ErrorF ("   Zero width or height\n");
	return NULL;
    }
    
    switch (bpp)
    {
    case 8:
	format = QXL_SURFACE_FMT_8_A;
	pformat = PIXMAN_a8;
	break;

    case 16:
	format = QXL_SURFACE_FMT_16_565;
	pformat = PIXMAN_r5g6b5;
	break;

    case 24:
	format = QXL_SURFACE_FMT_32_xRGB;
	pformat = PIXMAN_a8r8g8b8;
	break;
	
    case 32:
	format = QXL_SURFACE_FMT_32_ARGB;
	pformat = PIXMAN_a8r8g8b8;
	break;

    default:
      return NULL;
      break;
    }

    stride = width * PIXMAN_FORMAT_BPP (pformat) / 8;
    stride = (stride + 3) & ~3;

    /* the final + stride is to work around a bug where the device apparently 
     * scribbles after the end of the image
     */
retry2:
    surface->address = qxl_alloc (qxl->surf_mem, stride * height + stride);

    if (!surface->address)
    {
	ErrorF ("- %dth attempt\n", n_attempts);

	if (qxl_handle_oom (qxl) && ++n_attempts < 30000)
	    goto retry2;

	ErrorF ("Out of video memory: Could not allocate %lu bytes\n", stride * height + stride);
	
	return NULL;
    }

    surface->end = surface->address + stride * height;
#if 0
    ErrorF ("%d alloc address %lx from %p\n", surface->id, surface->address, qxl->surf_mem);
#endif
    
    cmd = make_surface_cmd (qxl, surface->id, QXL_SURFACE_CMD_CREATE);

    cmd->u.surface_create.format = format;
    cmd->u.surface_create.width = width;
    cmd->u.surface_create.height = height;
    cmd->u.surface_create.stride = - stride;

#if 0
    ErrorF ("stride: %d\n", stride);
#endif

    cmd->u.surface_create.physical = 
      physical_address (qxl, surface->address, qxl->vram_mem_slot);

#if 0
    ErrorF ("create %d\n", cmd->surface_id);
#endif
    
    push_surface_cmd (qxl, cmd);

    dev_addr = (uint32_t *)((uint8_t *)surface->address + stride * (height - 1));

    surface->dev_image = pixman_image_create_bits (
	pformat, width, height, dev_addr, - stride);

    surface->host_image = pixman_image_create_bits (
	pformat, width, height, NULL, -1);

    surface->Bpp = PIXMAN_FORMAT_BPP (pformat) / 8;
    
#if 0
    ErrorF ("   Allocating %d %lx\n", surface->id, surface->address);
#endif

    return surface;
}

void
qxl_surface_destroy (qxl_surface_t *surface)
{
    qxl_screen_t *qxl = surface->qxl;
    
#if 0
    ErrorF ("About to free %d\n", surface->id);
#endif

    if (--surface->ref_count == 0)
    {
	if (surface->dev_image)
	    pixman_image_unref (surface->dev_image);
	if (surface->host_image)
	    pixman_image_unref (surface->host_image);
	
	if (surface->id != 0)
	{
	    struct qxl_surface_cmd *cmd;
#if 0
	    ErrorF ("%d free address %lx from %p\n", surface->id, surface->address, surface->qxl->surf_mem);
#endif
	    cmd = make_surface_cmd (qxl, surface->id, QXL_SURFACE_CMD_DESTROY);
	    
#if 0
	    ErrorF ("  pushing destroy command %lx\n", cmd->release_info.id);
	    ErrorF ("destroy %d\n", cmd->surface_id);
#endif
	    
	    push_surface_cmd (qxl, cmd);
	}
    }
}

void
qxl_surface_unref (uint32_t id)
{
    qxl_surface_t *surface = all_surfaces + id;

    qxl_surface_destroy (surface);
}

void
qxl_surface_recycle (uint32_t id)
{
    qxl_surface_t *surface = all_surfaces + id;

    ErrorF ("recycle %d\n", id);
    
    qxl_free (surface->qxl->surf_mem, surface->address);
    surface_free (surface);
}

/* send anything pending to the other side */
void
qxl_surface_flush (qxl_surface_t *surface)
{
    ;
}


/* access */
#if 0
static void
download_box (qxl_screen_t *qxl, uint8_t *host,
	      int x1, int y1, int x2, int y2)
{
    struct qxl_ram_header *ram_header = (void *)((unsigned long)qxl->ram +
						 qxl->rom->ram_header_offset);
    int stride = - qxl->current_mode->stride;
    int Bpp = qxl->current_mode->bits / 8;
    uint8_t *host_line;
    uint8_t *dev_line;
    int height = y2 - y1;
    
#if 0
    ErrorF ("Downloading %d %d %d %d\n", x1, y1, x2, y2);
#endif
    
    ram_header->update_area.top = y1;
    ram_header->update_area.bottom = y2;
    ram_header->update_area.left = x1;
    ram_header->update_area.right = x2;
    ram_header->update_surface = 0;		/* Only primary for now */
    
    outb (qxl->io_base + QXL_IO_UPDATE_AREA, 0);
    
    dev_line = ((uint8_t *)qxl->ram) +
	(qxl->current_mode->y_res - 1) * (-stride) +	/* top of frame buffer */
	y1 * stride +					/* first line */
	x1 * Bpp;
    host_line = host + y1 * stride + x1 * Bpp;
    
#if 0
    ErrorF ("stride: %d\n", stride);
#endif
    
    while (height--)
    {
	uint8_t *h = host_line;
	uint8_t *d = dev_line;
	int w = (x2 - x1) * Bpp;
	
	host_line += stride;
	dev_line += stride;
	
	while (w--)
	    *h++ = *d++;
    }
}
#endif

static void
download_box (qxl_surface_t *surface, int x1, int y1, int x2, int y2)
{
    struct qxl_ram_header *ram_header = get_ram_header (surface->qxl);
    uint32_t before, after;

#if 0
    ErrorF ("Downloading %d %d %d %d\n", x1, y1, x2 - x1, y2 - y1);
#endif
    before = *((uint32_t *)surface->address - 1);
    
    ram_header->update_area.top = y1;
    ram_header->update_area.bottom = y2;
    ram_header->update_area.left = x1;
    ram_header->update_area.right = x2;
    ram_header->update_surface = surface->id;

    outb (surface->qxl->io_base + QXL_IO_UPDATE_AREA, 0);
    
    after = *((uint32_t *)surface->address - 1);

    if (surface->id != 0 && before != after)
      abort();

    pixman_color_t p = { 0xffff, 0x0000, 0xffff, 0xffff };
    pixman_image_t *pink = pixman_image_create_solid_fill (&p);

    pixman_image_composite (PIXMAN_OP_SRC, pink, NULL, surface->host_image,
			    0, 0, 0, 0, x1, y1, x2 - x1, y2 - y1);

    pixman_image_composite (PIXMAN_OP_SRC,
     			    surface->dev_image,
			    NULL,
			    surface->host_image,
			    x1, y1, 0, 0, x1, y1, x2 - x1, y2 - y1);
}

Bool
qxl_surface_prepare_access (qxl_surface_t  *surface,
			    PixmapPtr       pixmap,
			    RegionPtr       region,
			    uxa_access_t    access)
{
    int n_boxes;
    BoxPtr boxes;
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    RegionRec new;
    int stride, height;

    REGION_INIT (NULL, &new, (BoxPtr)NULL, 0);
    REGION_SUBTRACT (NULL, &new, region, &surface->access_region);

    region = &new;
    
    n_boxes = REGION_NUM_RECTS (region);
    boxes = REGION_RECTS (region);

#if 0
    ErrorF ("Preparing access to %d boxes\n", n_boxes);
#endif

    stride = pixman_image_get_stride (surface->dev_image);
    height = pixman_image_get_height (surface->dev_image);

#if 0
    ErrorF ("Flattening %p -> %p  (allocated end %p)\n", 
	    surface->address, 
	    surface->address + stride * height, surface->end);
#endif

    if (n_boxes < 25)
    {
	while (n_boxes--)
	{
	    download_box (surface, boxes->x1, boxes->y1, boxes->x2, boxes->y2);
	    
	    boxes++;
	}
    }
    else
    {
#if 0
	ErrorF ("Downloading extents (%d > %d)\n", n_boxes, 25);
#endif
	
	download_box (surface, new.extents.x1, new.extents.y1, new.extents.x2, new.extents.y2);
    }
    
    REGION_UNION (pScreen,
		  &(surface->access_region),
		  &(surface->access_region),
		      region);
    
    REGION_UNINIT (NULL, &new);
    
    pScreen->ModifyPixmapHeader(
	pixmap,
#if 0
	pixmap->drawable.width,
	pixmap->drawable.height,
#endif
	pixman_image_get_width (surface->host_image),
	pixman_image_get_height (surface->host_image),
	-1, -1, -1,
	pixman_image_get_data (surface->host_image));

    pixmap->devKind = pixman_image_get_stride (surface->host_image);

#if 0
    ErrorF ("stride %d\n", pixmap->devKind);
    ErrorF ("height %d\n", pixmap->drawable.height);
#endif
    
    return TRUE;
}

static void
translate_rect (struct qxl_rect *rect)
{
    rect->right -= rect->left;
    rect->bottom -= rect->top;
    rect->left = rect->top = 0;
}

static struct qxl_drawable *
make_drawable (qxl_screen_t *qxl, int surface, uint8_t type,
	       const struct qxl_rect *rect
	       /* , pRegion clip */)
{
    struct qxl_drawable *drawable;
    int i;
    
    drawable = qxl_allocnf (qxl, sizeof *drawable);
    
    drawable->release_info.id = pointer_to_u64 (drawable);
    
    drawable->type = type;
    
    drawable->surface_id = surface;		/* Only primary for now */
    drawable->effect = QXL_EFFECT_OPAQUE;
    drawable->self_bitmap = 0;
    drawable->self_bitmap_area.top = 0;
    drawable->self_bitmap_area.left = 0;
    drawable->self_bitmap_area.bottom = 0;
    drawable->self_bitmap_area.right = 0;
    /* FIXME: add clipping */
    drawable->clip.type = QXL_CLIP_TYPE_NONE;
    
    /* FIXME: What are you supposed to put in surfaces_dest and surfaces_rects? */
    for (i = 0; i < 3; ++i)
	drawable->surfaces_dest[i] = -1;
    
    if (rect)
	drawable->bbox = *rect;
    
    drawable->mm_time = qxl->rom->mm_clock;
    
    return drawable;
}

static void
push_drawable (qxl_screen_t *qxl, struct qxl_drawable *drawable)
{
    struct qxl_command cmd;
    
    /* When someone runs "init 3", the device will be 
     * switched into VGA mode and there is nothing we
     * can do about it. We get no notification.
     * 
     * However, if commands are submitted when the device
     * is in VGA mode, they will be queued up, and then
     * the next time a mode set set, an assertion in the
     * device will take down the entire virtual machine.
     */
    if (!in_vga_mode (qxl))
    {
	cmd.type = QXL_CMD_DRAW;
	cmd.data = physical_address (qxl, drawable, qxl->main_mem_slot);
	
	qxl_ring_push (qxl->command_ring, &cmd);
    }
}

enum ROPDescriptor
{
    ROPD_INVERS_SRC = (1 << 0),
    ROPD_INVERS_BRUSH = (1 << 1),
    ROPD_INVERS_DEST = (1 << 2),
    ROPD_OP_PUT = (1 << 3),
    ROPD_OP_OR = (1 << 4),
    ROPD_OP_AND = (1 << 5),
    ROPD_OP_XOR = (1 << 6),
    ROPD_OP_BLACKNESS = (1 << 7),
    ROPD_OP_WHITENESS = (1 << 8),
    ROPD_OP_INVERS = (1 << 9),
    ROPD_INVERS_RES = (1 <<10),
};

static void
submit_fill (qxl_screen_t *qxl, int id,
	     const struct qxl_rect *rect, uint32_t color)
{
    struct qxl_drawable *drawable;
    
    drawable = make_drawable (qxl, id, QXL_DRAW_FILL, rect);
    
    drawable->u.fill.brush.type = QXL_BRUSH_TYPE_SOLID;
    drawable->u.fill.brush.u.color = color;
    drawable->u.fill.rop_descriptor = ROPD_OP_PUT;
    drawable->u.fill.mask.flags = 0;
    drawable->u.fill.mask.pos.x = 0;
    drawable->u.fill.mask.pos.y = 0;
    drawable->u.fill.mask.bitmap = 0;
    
    push_drawable (qxl, drawable);
}

static void
upload_box (qxl_surface_t *surface, int x1, int y1, int x2, int y2)
{
    struct qxl_rect rect;
    struct qxl_drawable *drawable;
    struct qxl_image *image;
    qxl_screen_t *qxl = surface->qxl;
    uint32_t *data;
    int stride;
    
    rect.left = x1;
    rect.right = x2;
    rect.top = y1;
    rect.bottom = y2;
    
#if 0
#endif
    
    /* if (surface->id != 0) */
    /* {  */
    /* 	/\* Paint a green flash after uploading *\/ */
    /* 	submit_fill (qxl, surface->id, &rect, rand()); */
    /* } */

    drawable = make_drawable (qxl, surface->id, QXL_DRAW_COPY, &rect);
    drawable->u.copy.src_area = rect;
    translate_rect (&drawable->u.copy.src_area);
    drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
    drawable->u.copy.scale_mode = 0;
    drawable->u.copy.mask.flags = 0;
    drawable->u.copy.mask.pos.x = 0;
    drawable->u.copy.mask.pos.y = 0;
    drawable->u.copy.mask.bitmap = 0;

    data = pixman_image_get_data (surface->host_image);
    stride = pixman_image_get_stride (surface->host_image);
    
    image = qxl_image_create (
	qxl, (const uint8_t *)data, x1, y1, x2 - x1, y2 - y1, stride, 
	surface->Bpp);
    drawable->u.copy.src_bitmap =
	physical_address (qxl, image, qxl->main_mem_slot);
    
    push_drawable (qxl, drawable);

    /* if (surface->id != 0) */
    /* {  */
    /* 	/\* Paint a green flash after uploading *\/ */
    /* 	submit_fill (qxl, surface->id, &rect, rand()); */
    /* } */
}

void
qxl_surface_finish_access (qxl_surface_t *surface, PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    int w = pixmap->drawable.width;
    int h = pixmap->drawable.height;
    int n_boxes;
    BoxPtr boxes;

    n_boxes = REGION_NUM_RECTS (&surface->access_region);
    boxes = REGION_RECTS (&surface->access_region);

    if (n_boxes < 25)
    {
	while (n_boxes--)
	{
	    upload_box (surface, boxes->x1, boxes->y1, boxes->x2, boxes->y2);
	
	    boxes++;
	}
    }
    else
    {
	upload_box (surface,
		    surface->access_region.extents.x1,
		    surface->access_region.extents.y1,
		    surface->access_region.extents.x2,
		    surface->access_region.extents.y2);
    }

    REGION_EMPTY (pScreen, &surface->access_region);
    
    pScreen->ModifyPixmapHeader(pixmap, w, h, -1, -1, 0, NULL);
}


/* solid */
Bool
qxl_surface_prepare_solid (qxl_surface_t *destination,
			   Pixel	  fg)
{
    destination->u.solid_pixel = fg; //  ^ (rand() >> 16);

    return TRUE;
}

void
qxl_surface_solid (qxl_surface_t *destination,
		   int	          x1,
		   int	          y1,
		   int	          x2,
		   int	          y2)
{
    qxl_screen_t *qxl = destination->qxl;
    struct qxl_rect qrect;

    qrect.top = y1;
    qrect.bottom = y2;
    qrect.left = x1;
    qrect.right = x2;
    
    submit_fill (qxl, destination->id, &qrect, destination->u.solid_pixel);
}


/* copy */
Bool
qxl_surface_prepare_copy (qxl_surface_t *dest,
			  qxl_surface_t *source)
{
    dest->u.copy_src = source;
    return TRUE;
}

void
qxl_surface_copy (qxl_surface_t *dest,
		  int  src_x1, int src_y1,
		  int  dest_x1, int dest_y1,
		  int width, int height)
{
    qxl_screen_t *qxl = dest->qxl;
    struct qxl_drawable *drawable;
    struct qxl_rect qrect;

    qrect.top = dest_y1;
    qrect.bottom = dest_y1 + height;
    qrect.left = dest_x1;
    qrect.right = dest_x1 + width;
    
    if (dest->id == dest->u.copy_src->id)
    {
/* 	    ErrorF ("   Translate %d %d %d %d by %d %d (offsets %d %d)\n", */
/* 		    b->x1, b->y1, b->x2, b->y2, */
/* 		    dx, dy, dst_xoff, dst_yoff); */
	
	drawable = make_drawable (qxl, dest->id, QXL_COPY_BITS, &qrect);
	drawable->u.copy_bits.src_pos.x = src_x1;
	drawable->u.copy_bits.src_pos.y = src_y1;
    }
    else
    {
	struct qxl_image *image = qxl_allocnf (qxl, sizeof *image);

#if 0
	ErrorF ("Copy  %d to %d\n", dest->u.copy_src->id, dest->id);
#endif

	dest->u.copy_src->ref_count++;
    
	image->descriptor.id = 0;
	image->descriptor.type = QXL_IMAGE_TYPE_SURFACE;
	image->descriptor.width = 0;
	image->descriptor.height = 0;
	image->u.surface_id = dest->u.copy_src->id;

	drawable = make_drawable (qxl, dest->id, QXL_DRAW_COPY, &qrect);

	drawable->u.copy.src_bitmap = physical_address (qxl, image, qxl->main_mem_slot);
	drawable->u.copy.src_area.left = src_x1;
	drawable->u.copy.src_area.top = src_y1;
	drawable->u.copy.src_area.right = src_x1 + width;
	drawable->u.copy.src_area.bottom = src_y1 + height;
	drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
	drawable->u.copy.scale_mode = 0;
	drawable->u.copy.mask.flags = 0;
	drawable->u.copy.mask.pos.x = 0;
	drawable->u.copy.mask.pos.y = 0;
	drawable->u.copy.mask.bitmap = 0;
    }

    push_drawable (qxl, drawable);
}
