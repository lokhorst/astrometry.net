/*
 This file is part of libkd.
 Copyright 2008 Dustin Lang.

 libkd is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, version 2.

 libkd is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with libkd; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <string.h>

#include "kdtree.h"
#include "kdtree_fits_io.h"
#include "ioutils.h"
#include "fitsioutils.h"
#include "errors.h"

void printHelp(char* progname) {
	printf("\nUsage: %s <input> <output>\n"
		   "\n", progname);
}

extern char *optarg;
extern int optind, opterr, optopt;

const char* OPTIONS = "h";

int main(int argc, char** args) {
    int argchar;
	char* progname = args[0];
	kdtree_t* kd;
	char* infn;
	char* outfn;
    qfits_header* hdr;
    qfits_header* outhdr;
    int i, Next;
    FILE* fout;
    FILE* fin;

    while ((argchar = getopt(argc, args, OPTIONS)) != -1)
        switch (argchar) {
		case 'h':
			printHelp(progname);
			exit(-1);
		}

    if (optind != argc - 2) {
        printHelp(progname);
        exit(-1);
    }

    infn = args[optind];
    outfn = args[optind+1];

    printf("Reading kdtree from file %s ...\n", infn);

    {
        err_t* err;
        errors_push_state();
        err = errors_get_state();
        err->print = NULL;
        err->save = TRUE;

        kd = kdtree_fits_read(infn, NULL, &hdr);

        if (!kd) {
            ERROR("Failed to read kdtree from file %s", infn);
            error_print_stack(err, stderr);
            errors_free();
            exit(-1);
        }

        errors_pop_state();
    }

    printf("Tree name: %s\n", kd->name);
    printf("Treetype: 0x%x\n", kd->treetype);
    printf("Data type:     %s\n", kdtree_kdtype_to_string(kdtree_datatype(kd)));
    printf("Tree type:     %s\n", kdtree_kdtype_to_string(kdtree_treetype(kd)));
    printf("External type: %s\n", kdtree_kdtype_to_string(kdtree_exttype(kd)));

    printf("N data points:  %i\n", kd->ndata);
    printf("Dimensions:     %i\n", kd->ndim);
    printf("Nodes:          %i\n", kd->nnodes);
    printf("Leaf nodes:     %i\n", kd->nbottom);
    printf("Non-leaf nodes: %i\n", kd->ninterior);
    printf("Tree levels:    %i\n", kd->nlevels);

    printf("Legacy nodes: %s\n", (kd->nodes  ? "yes" : "no"));
    printf("LR array:     %s\n", (kd->lr     ? "yes" : "no"));
    printf("Perm array:   %s\n", (kd->perm   ? "yes" : "no"));
    printf("Bounding box: %s\n", (kd->bb.any ? "yes" : "no"));
    printf("Split plane:  %s\n", (kd->split.any ? "yes" : "no"));
    printf("Split dim:    %s\n", (kd->splitdim  ? "yes" : "no"));
    printf("Data:         %s\n", (kd->data.any  ? "yes" : "no"));

    if (kd->minval && kd->maxval) {
        int d;
        printf("Data ranges:\n");
        for (d=0; d<kd->ndim; d++)
            printf("  %i: [%g, %g]\n", d, kd->minval[d], kd->maxval[d]);
    }

    printf("Computing bounding boxes...\n");
    kdtree_fix_bounding_boxes(kd);

    printf("Running kdtree_check...\n");
    if (kdtree_check(kd)) {
        printf("kdtree_check failed.\n");
        exit(-1);
    }

    outhdr = qfits_header_new();
    fits_append_long_comment(outhdr, "This file was processed by the fix-bb "
                             "program, part of the Astrometry.net suite.  The "
                             "extra FITS headers in the original file are "
                             "given below:");
    fits_append_long_comment(outhdr, "---------------------------------");
                          
    for (i=0; i<hdr->n; i++) {
        char key[FITS_LINESZ+1];
        char val[FITS_LINESZ+1];
        char com[FITS_LINESZ+1];
        qfits_header_getitem(hdr, i, key, val, com, NULL);
        if (!(fits_is_primary_header(key) ||
              fits_is_table_header(key))) {
            qfits_header_append(outhdr, key, val, com, NULL);
        }
    }
    fits_append_long_comment(outhdr, "---------------------------------");

    kd->name = strdup("tst");

    if (kdtree_fits_write(kd, outfn, outhdr)) {
        ERROR("Failed to write output");
        exit(-1);
    }

    printf("Finding extra extensions...\n");
    Next = qfits_query_n_ext(infn);

    fin = fopen(infn, "rb");
    if (!fin) {
        SYSERROR("Failed to re-open input file %s for reading", infn);
        exit(-1);
    }
    fout = fopen(outfn, "ab");
    if (!fout) {
        SYSERROR("Failed to re-open output file %s for writing", outfn);
        exit(-1);
    }

    for (i=0; i<Next; i++) {
        int hoffset, hlength;
        int doffset, dlength;
        int ext = i+1;

        if (qfits_is_table(infn, ext)) {
            qfits_table* table;
            table = qfits_table_open(infn, ext);
            if (table &&
                (table->nc == 1) &&
                kdtree_fits_column_is_kdtree(table->col[0].tlabel))
                continue;
        }
        printf("Extension %i is not part of the kdtree.  Copying it verbatim.\n", ext);
        if (qfits_get_hdrinfo(infn, ext, &hoffset, &hlength) ||
            qfits_get_datinfo(infn, ext, &doffset, &dlength)) {
            ERROR("Failed to get header or data offset & length for extension %i", ext);
            exit(-1);
        }

        if (pipe_file_offset(fin, hoffset, hlength, fout) ||
            pipe_file_offset(fin, doffset, dlength, fout)) {
            ERROR("Failed to write extension %i verbatim", ext);
            exit(-1);
        }
    }
    fclose(fin);
    if (fclose(fout)) {
        SYSERROR("Failed to close output file %s", outfn);
        exit(-1);
    }

    kdtree_fits_close(kd);
    errors_free();
	return 0;
}
