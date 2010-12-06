/*
 * pdfdraw -- command line tool for drawing pdf documents
 */

#include "fitz.h"
#include "mupdf.h"

/* SumatraPDF: add support for GDI+ draw device */
#ifdef _WIN32
#include <windows.h>
#define GDI_PLUS_BMP_RENDERER
#else
#include <sys/time.h>
#endif

char *output = NULL;
float resolution = 72;

int showxml = 0;
int showtext = 0;
int showtime = 0;
int showmd5 = 0;
int savealpha = 0;
int uselist = 1;

fz_colorspace *colorspace;
fz_glyphcache *glyphcache;
char *filename;

struct {
	int count, total;
	int min, max;
	int minpage, maxpage;
} timing;

static void die(fz_error error)
{
	fz_catch(error, "aborting");
	exit(1);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: pdfdraw [options] input.pdf [pages]\n"
		"\t-o -\toutput filename (%%d for page number)\n"
#ifdef GDI_PLUS_BMP_RENDERER
		"\t\tsupported formats: pgm, ppm, pam, png, bmp\n"
#else
		"\t\tsupported formats: pgm, ppm, pam, png\n"
#endif
		"\t-p -\tpassword\n"
		"\t-r -\tresolution in dpi (default: 72)\n"
		"\t-A\tdisable accelerated functions\n"
		"\t-a\tsave alpha channel (only pam and png)\n"
		"\t-g\trender in grayscale\n"
		"\t-m\tshow timing information\n"
		"\t-t\tshow text (-tt for xml)\n"
		"\t-x\tshow display list\n"
		"\t-d\tdisable use of display list\n"
		"\t-5\tshow md5 checksums\n"
		"\tpages\tcomma separated list of ranges\n");
	exit(1);
}

static int gettime(void)
{
	static struct timeval first;
	static int once = 1;
	struct timeval now;
	if (once)
	{
		gettimeofday(&first, NULL);
		once = 0;
	}
	gettimeofday(&now, NULL);
	return (now.tv_sec - first.tv_sec) * 1000 + (now.tv_usec - first.tv_usec) / 1000;
}

static int isrange(char *s)
{
	while (*s)
	{
		if ((*s < '0' || *s > '9') && *s != '-' && *s != ',')
			return 0;
		s++;
	}
	return 1;
}

#ifdef GDI_PLUS_BMP_RENDERER
static void drawbmp(pdf_xref *xref, pdf_page *page, fz_displaylist *list, int pagenum)
{
	float zoom;
	fz_matrix ctm;
	fz_bbox bbox;

	int w, h;
	fz_device *dev;
	HDC hDC, hDCMain;
	RECT rc;
	HBRUSH bgBrush;
	HBITMAP hbmp;
	BITMAPINFO bmi = { 0 };
	int bmpDataLen;
	char *bmpData;

	zoom = resolution / 72;
	ctm = fz_translate(-page->mediabox.x0, -page->mediabox.y1);
	ctm = fz_concat(ctm, fz_scale(zoom, -zoom));
	ctm = fz_concat(ctm, fz_rotate(page->rotate));
	bbox = fz_roundrect(fz_transformrect(ctm, page->mediabox));

	ctm = fz_concat(ctm, fz_translate(-bbox.x0, -bbox.y0));
	w = bbox.x1 - bbox.x0;
	h = bbox.y1 - bbox.y0;

	hDCMain = GetDC(NULL);
	hDC = CreateCompatibleDC(hDCMain);
	hbmp = CreateCompatibleBitmap(hDCMain, w, h);
	DeleteObject(SelectObject(hDC, hbmp));

	SetRect(&rc, 0, 0, w, h);
	bgBrush = CreateSolidBrush(RGB(0xFF,0xFF,0xFF));
	FillRect(hDC, &rc, bgBrush);
	DeleteObject(bgBrush);

	bbox.x0 = 0; bbox.x1 = w;
	bbox.y0 = 0; bbox.y1 = h;

	dev = fz_newgdiplusdevice(hDC, bbox);
	if (list)
		fz_executedisplaylist2(list, dev, ctm, fz_roundrect(page->mediabox));
	else
		pdf_runpage(xref, page, dev, ctm);
	fz_freedevice(dev);

	bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
	bmi.bmiHeader.biHeight = h;
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;

	bmpDataLen = ((w * 3 + 3) / 4) * 4 * h;
	bmpData = fz_malloc(bmpDataLen);
	if (!GetDIBits(hDC, hbmp, 0, h, bmpData, &bmi, DIB_RGB_COLORS))
		die(fz_throw("gdierror: cannot draw page %d in PDF file '%s'", pagenum, filename));

	DeleteDC(hDC);
	ReleaseDC(NULL, hDCMain);
	DeleteObject(hbmp);

	if (output)
	{
		char buf[512];
		int fd;
		BITMAPFILEHEADER bmpfh = { 0 };

		sprintf(buf, output, pagenum);
		fd = open(buf, O_BINARY|O_WRONLY|O_CREAT|O_TRUNC, 0666);
		if (fd < 0)
			die(fz_throw("ioerror: could not create raster file '%s'", buf));

		bmpfh.bfType = MAKEWORD('B', 'M');
		bmpfh.bfOffBits = sizeof(bmpfh) + sizeof(bmi);
		bmpfh.bfSize = bmpfh.bfOffBits + bmpDataLen;

		write(fd, &bmpfh, sizeof(bmpfh));
		write(fd, &bmi, sizeof(bmi));
		write(fd, bmpData, bmpDataLen);

		close(fd);
	}

	if (showmd5)
	{
		fz_md5 md5;
		unsigned char digest[16];
		int i;

		fz_md5init(&md5);
		fz_md5update(&md5, bmpData, bmpDataLen);
		fz_md5final(&md5, digest);

		printf(" ");
		for (i = 0; i < 16; i++)
			printf("%02x", digest[i]);
	}

	fz_free(bmpData);
}
#endif

static void drawpage(pdf_xref *xref, int pagenum)
{
	fz_error error;
	fz_obj *pageobj;
	pdf_page *page;
	fz_displaylist *list;
	fz_device *dev;
	int start;

	if (showtime)
	{
		start = gettime();
	}

	pageobj = pdf_getpageobject(xref, pagenum);
	error = pdf_loadpage(&page, xref, pageobj);
	if (error)
		die(fz_rethrow(error, "cannot load page %d in file '%s'", pagenum, filename));

	list = nil;

	if (uselist)
	{
		list = fz_newdisplaylist();
		dev = fz_newlistdevice(list);
		error = pdf_runpage(xref, page, dev, fz_identity);
		if (error)
			die(fz_rethrow(error, "cannot draw page %d in file '%s'", pagenum, filename));
		fz_freedevice(dev);
	}

	if (showxml)
	{
		dev = fz_newtracedevice();
		printf("<page number=\"%d\">\n", pagenum);
		if (list)
			fz_executedisplaylist(list, dev, fz_identity);
		else
			pdf_runpage(xref, page, dev, fz_identity);
		printf("</page>\n");
		fz_freedevice(dev);
	}

	if (showtext)
	{
		fz_textspan *text = fz_newtextspan();
		dev = fz_newtextdevice(text);
		if (list)
			fz_executedisplaylist(list, dev, fz_identity);
		else
			pdf_runpage(xref, page, dev, fz_identity);
		fz_freedevice(dev);
		printf("[Page %d]\n", pagenum);
		if (showtext > 1)
			fz_debugtextspanxml(text);
		else
			fz_debugtextspan(text);
		printf("\n");
		fz_freetextspan(text);
	}

	if (showmd5 || showtime)
		printf("page %s %d", filename, pagenum);

#ifdef GDI_PLUS_BMP_RENDERER
	if (output && strstr(output, ".bmp"))
		drawbmp(xref, page, list, pagenum);
	else
#endif
	if (output || showmd5 || showtime)
	{
		float zoom;
		fz_matrix ctm;
		fz_bbox bbox;
		fz_pixmap *pix;

		zoom = resolution / 72;
		ctm = fz_translate(0, -page->mediabox.y1);
		ctm = fz_concat(ctm, fz_scale(zoom, -zoom));
		ctm = fz_concat(ctm, fz_rotate(page->rotate));
		bbox = fz_roundrect(fz_transformrect(ctm, page->mediabox));

		/* TODO: banded rendering and multi-page ppm */

		pix = fz_newpixmapwithrect(colorspace, bbox);

		if (savealpha)
			fz_clearpixmap(pix);
		else
			fz_clearpixmapwithcolor(pix, 255);

		dev = fz_newdrawdevice(glyphcache, pix);
		if (list)
			fz_executedisplaylist(list, dev, ctm);
		else
			pdf_runpage(xref, page, dev, ctm);
		fz_freedevice(dev);

		if (output)
		{
			char buf[512];
			sprintf(buf, output, pagenum);
			if (strstr(output, ".pgm") || strstr(output, ".ppm") || strstr(output, ".pnm"))
				fz_writepnm(pix, buf);
			else if (strstr(output, ".pam"))
				fz_writepam(pix, buf, savealpha);
			else if (strstr(output, ".png"))
				fz_writepng(pix, buf, savealpha);
		}

		if (showmd5)
		{
			fz_md5 md5;
			unsigned char digest[16];
			int i;

			fz_md5init(&md5);
			fz_md5update(&md5, pix->samples, pix->w * pix->h * pix->n);
			fz_md5final(&md5, digest);

			printf(" ");
			for (i = 0; i < 16; i++)
				printf("%02x", digest[i]);
		}

		fz_droppixmap(pix);
	}

	if (list)
		fz_freedisplaylist(list);

	pdf_freepage(page);

	if (showtime)
	{
		int end = gettime();
		int diff = end - start;

		if (diff < timing.min)
		{
			timing.min = diff;
			timing.minpage = pagenum;
		}
		if (diff > timing.max)
		{
			timing.max = diff;
			timing.maxpage = pagenum;
		}
		timing.total += diff;
		timing.count ++;

		printf(" %dms", diff);
	}

	if (showmd5 || showtime)
		printf("\n");

	pdf_agestore(xref->store, 3);
}

static void drawrange(pdf_xref *xref, char *range)
{
	int page, spage, epage;
	char *spec, *dash;

	spec = fz_strsep(&range, ",");
	while (spec)
	{
		dash = strchr(spec, '-');

		if (dash == spec)
			spage = epage = 1;
		else
			spage = epage = atoi(spec);

		if (dash)
		{
			if (strlen(dash) > 1)
				epage = atoi(dash + 1);
			else
				epage = pdf_getpagecount(xref);
		}

		spage = CLAMP(spage, 1, pdf_getpagecount(xref));
		epage = CLAMP(epage, 1, pdf_getpagecount(xref));

		if (spage < epage)
			for (page = spage; page <= epage; page++)
				drawpage(xref, page);
		else
			for (page = spage; page >= epage; page--)
				drawpage(xref, page);

		spec = fz_strsep(&range, ",");
	}
}

int main(int argc, char **argv)
{
	char *password = "";
	int grayscale = 0;
	int accelerate = 1;
	pdf_xref *xref;
	fz_error error;
	int c;

	while ((c = fz_getopt(argc, argv, "o:p:r:Aadgmtx5")) != -1)
	{
		switch (c)
		{
		case 'o': output = fz_optarg; break;
		case 'p': password = fz_optarg; break;
		case 'r': resolution = atof(fz_optarg); break;
		case 'A': accelerate = 0; break;
		case 'a': savealpha = 1; break;
		case 'm': showtime++; break;
		case 't': showtext++; break;
		case 'x': showxml++; break;
		case '5': showmd5++; break;
		case 'g': grayscale++; break;
		case 'd': uselist = 0; break;
		default: usage(); break;
		}
	}

	if (fz_optind == argc)
		usage();

	if (!showtext && !showxml && !showtime && !showmd5 && !output)
	{
		printf("nothing to do\n");
		exit(0);
	}

	if (accelerate)
		fz_accelerate();

	glyphcache = fz_newglyphcache();

	colorspace = fz_devicergb;
	if (grayscale)
		colorspace = fz_devicegray;
	if (output && strstr(output, ".pgm"))
		colorspace = fz_devicegray;
	if (output && strstr(output, ".ppm"))
		colorspace = fz_devicergb;

	timing.count = 0;
	timing.total = 0;
	timing.min = 1 << 30;
	timing.max = 0;
	timing.minpage = 0;
	timing.maxpage = 0;

	if (showxml)
		printf("<?xml version=\"1.0\"?>\n");

	while (fz_optind < argc)
	{
		filename = argv[fz_optind++];

		error = pdf_openxref(&xref, filename, password);
		if (error)
			die(fz_rethrow(error, "cannot open document: %s", filename));

		error = pdf_loadpagetree(xref);
		if (error)
			die(fz_rethrow(error, "cannot load page tree: %s", filename));

		if (showxml)
			printf("<document name=\"%s\">\n", filename);

		if (fz_optind == argc || !isrange(argv[fz_optind]))
			drawrange(xref, "1-");
		if (fz_optind < argc && isrange(argv[fz_optind]))
			drawrange(xref, argv[fz_optind++]);

		if (showxml)
			printf("</document>\n");

		pdf_freexref(xref);
	}

	if (showtime)
	{
		printf("total %dms / %d pages for an average of %dms\n",
			timing.total, timing.count, timing.total / timing.count);
		printf("fastest page %d: %dms\n", timing.minpage, timing.min);
		printf("slowest page %d: %dms\n", timing.maxpage, timing.max);
	}

	fz_freeglyphcache(glyphcache);

	return 0;
}
