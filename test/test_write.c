/*
Copyright (c) 2016, Lawrence Livermore National Security, LLC.
Produced at the Lawrence Livermore National Laboratory
Written by Mark C. Miller, miller86@llnl.gov
LLNL-CODE-707197. All rights reserved.

This file is part of H5Z-ZFP. Please also read the BSD license
https://raw.githubusercontent.com/LLNL/H5Z-ZFP/master/LICENSE 
*/

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"

#ifdef H5Z_ZFP_USE_PLUGIN
#include "H5Zzfp_plugin.h"
#else
#include "H5Zzfp_lib.h"
#include "H5Zzfp_props.h"
#endif

#define NAME_LEN 256

/* convenience macro to handle command-line args and help */
#define HANDLE_ARG(A,PARSEA,PRINTA,HELPSTR)                     \
{                                                               \
    int i;                                                      \
    char tmpstr[64];                                            \
    int len;                                                    \
    int len2 = strlen(#A)+1;                                    \
    for (i = 0; i < argc; i++)                                  \
    {                                                           \
        if (!strncmp(argv[i], #A"=", len2))                     \
        {                                                       \
            A = PARSEA;                                         \
            break;                                              \
        }                                                       \
        else if (!strncasecmp(argv[i], "help", 4))              \
        {                                                       \
            return 0;                                           \
        }                                                       \
    }                                                           \
    len = snprintf(tmpstr, sizeof(tmpstr), "%s=" PRINTA, #A, A);\
    printf("    %s%*s\n",tmpstr,60-len,#HELPSTR);               \
}


/* convenience macro to handle errors */
#define ERROR(FNAME)                                              \
do {                                                              \
    int _errno = errno;                                           \
    fprintf(stderr, #FNAME " failed at line %d, errno=%d (%s)\n", \
        __LINE__, _errno, _errno?strerror(_errno):"ok");          \
    return 1;                                                     \
} while(0)

/* Generate a simple, 1D sinusioidal data array with some noise */
#define TYPINT 1
#define TYPDBL 2
static int gen_data(size_t npoints, double noise, double amp, void **_buf, int typ)
{
    size_t i;
    double *pdbl = 0;
    int *pint = 0;

    /* create data buffer to write */
    if (typ == TYPINT)
        pint = (int *) malloc(npoints * sizeof(int));
    else
        pdbl = (double *) malloc(npoints * sizeof(double));
    srandom(0xDeadBeef);
    for (i = 0; i < npoints; i++)
    {
        double x = 2 * M_PI * (double) i / (double) (npoints-1);
        double n = noise * ((double) random() / ((double)(1<<31)-1) - 0.5);
        if (typ == TYPINT)
            pint[i] = (int) (amp * (1 + sin(x)) + n);
        else
            pdbl[i] = (double) (amp * (1 + sin(x)) + n);
    }
    if (typ == TYPINT)
        *_buf = pint;
    else
        *_buf = pdbl;
    return 0;
}

/* Populate the hyper-dimensional array with samples of a radially symmetric
   sinc() function but where certain sub-spaces are randomized through dimindx arrays */
static void
hyper_smooth_radial(void *b, int typ, int n, int ndims, int const *dims, int const *m,
    int const * const dimindx[10])
{
    int i;
    double hyper_radius = 0;
    const double amp = 10000;
    double val;

    for (i = ndims-1; i >= 0; i--)
    {
        int iar = n / m[i];
        iar = dimindx[i][iar]; /* allow for randomized shuffle of this axis */
        iar -= dims[i]/2;      /* ensure centering in middle of the array */
        n = n % m[i];
        hyper_radius += iar*iar;
    }
    hyper_radius = sqrt(hyper_radius);

    if (hyper_radius < 1e-15)
        val = amp;
    else
        val = amp * sin(0.4*hyper_radius) / (0.4*hyper_radius);

    if (typ == TYPINT)
    {
        int *pi = (int*) b;
        *pi = (int) val;
    }
    else
    {
        double *pd = (double*) b;
        *pd = val;
    }
}

static double func(int i, double arg)
{
    /* a random assortment of interesting, somewhat bounded, unary functions */
    double (*const funcs[])(double x) = {cos, j0, fabs, sin, cbrt, erf};
    int const nfuncs = sizeof(funcs)/sizeof(funcs[0]);
    return funcs[i%nfuncs](arg);
}

/* Populate the hyper-dimensional array with samples of set of seperable functions
   but where certain sub-spaces are randomized through dimindx arrays */
static void
hyper_smooth_separable(void *b, int typ, int n, int ndims, int const *dims, int const *m,
    int const * const dimindx[10])
{
    int i;
    double val = 1;

    for (i = ndims-1; i >= 0; i--)
    {
        int iar = n / m[i];
        iar = dimindx[i][iar]; /* allow for randomized shuffle of this axis */
        iar -= dims[i]/2;      /* ensure centering in middle of the array */
        n = n % m[i];
        val *= func(i, (double) iar);
    }

    if (typ == TYPINT)
    {
        int *pi = (int*) b;
        *pi = (int) val;
    }
    else
    {
        double *pd = (double*) b;
        *pd = val;
    }
}

/* Produce multi-dimensional array test data with the property that it is random
   in the UNcorrelated dimensions but smooth in the correlated dimensions. This
   is achieved by randomized shuffling of the array indices used in specific
   dimensional axes of the array. */
static void *
gen_random_correlated_array(int typ, int ndims, int const *dims, int nucdims, int const *ucdims)
{
    int i, n;
    int nbyt = (int) (typ == TYPINT ? sizeof(int) : sizeof(double)); 
    unsigned char *buf, *buf0;
    int m[10]; /* subspace multipliers */
    int *dimindx[10];
   
    assert(ndims <= 10);

    /* Set up total size and sub-space multipliers */
    for (i=0, n=1; i < ndims; i++)
    {
        n *= dims[i];
        m[i] = i==0?1:m[i-1]*dims[i-1];
    }

    /* allocate buffer of suitable size (doubles or ints) */
    buf0 = buf = (unsigned char*) malloc(n * nbyt);
    
    /* set up dimension identity indexing (e.g. Idx[i]==i) so that
       we can randomize those dimenions we wish to have UNcorrelated */
    for (i = 0; i < ndims; i++)
    {
        int j;
        dimindx[i] = (int*) malloc(dims[i]*sizeof(int));
        for (j = 0; j < dims[i]; j++)
            dimindx[i][j] = j;
    }

    /* Randomize selected dimension indexing */
    srandom(0xDeadBeef);
    for (i = 0; i < nucdims; i++)
    {
        int j, ucdimi = ucdims[i];
        for (j = 0; j < dims[ucdimi]-1; j++)
        {
            int tmp, k = random() % (dims[ucdimi]-j);
            if (k == j) continue;
            tmp = dimindx[ucdimi][j];
            dimindx[ucdimi][j] = k;
            dimindx[ucdimi][k] = tmp;
        }
    }

    /* populate the array data */
    for (i = 0; i < n; i++)
    {
        hyper_smooth_separable(buf, typ, i, ndims, dims, m, (int const * const *) dimindx);
        buf += nbyt;
    }

    /* free dimension indexing */
    for (i = 0; i < ndims; i++)
        free(dimindx[i]);

    return buf0;
}


static int read_data(char const *fname, size_t npoints, double **_buf)
{
    size_t const nbytes = npoints * sizeof(double);
    int fd;

    if (0 > (fd = open(fname, O_RDONLY))) ERROR(open);
    if (0 == (*_buf = (double *) malloc(nbytes))) ERROR(malloc);
    if (nbytes != read(fd, *_buf, nbytes)) ERROR(read);
    if (0 != close(fd)) ERROR(close);
    return 0;
}

static hid_t setup_filter(int n, hsize_t *chunk, int zfpmode,
    double rate, double acc, uint prec,
    uint minbits, uint maxbits, uint maxprec, int minexp)
{
    hid_t cpid;
    unsigned int cd_values[10];
    int i, cd_nelmts = 10;

    /* setup dataset creation properties */
    if (0 > (cpid = H5Pcreate(H5P_DATASET_CREATE))) ERROR(H5Pcreate);
    if (0 > H5Pset_chunk(cpid, n, chunk)) ERROR(H5Pset_chunk);

#ifdef H5Z_ZFP_USE_PLUGIN
    /* setup zfp filter via generic (cd_values) interface */
    if (zfpmode == H5Z_ZFP_MODE_RATE)
        H5Pset_zfp_rate_cdata(rate, cd_nelmts, cd_values);
    else if (zfpmode == H5Z_ZFP_MODE_PRECISION)
        H5Pset_zfp_precision_cdata(prec, cd_nelmts, cd_values);
    else if (zfpmode == H5Z_ZFP_MODE_ACCURACY)
        H5Pset_zfp_accuracy_cdata(acc, cd_nelmts, cd_values);
    else if (zfpmode == H5Z_ZFP_MODE_EXPERT)
        H5Pset_zfp_expert_cdata(minbits, maxbits, maxprec, minexp, cd_nelmts, cd_values);
    else
        cd_nelmts = 0; /* causes default behavior of ZFP library */

    /* print cd-values array used for filter */
    printf("%d cd_values= ",cd_nelmts);
    for (i = 0; i < cd_nelmts; i++)
        printf("%u,", cd_values[i]);
    printf("\n");

    /* Add filter to the pipeline via generic interface */
    if (0 > H5Pset_filter(cpid, H5Z_FILTER_ZFP, H5Z_FLAG_MANDATORY, cd_nelmts, cd_values)) ERROR(H5Pset_filter);

#else 

    /* When filter is used as a library, we need to init it */
    H5Z_zfp_initialize();

    /* Setup the filter using properties interface. These calls also add
       the filter to the pipeline */
    if (zfpmode == H5Z_ZFP_MODE_RATE)
        H5Pset_zfp_rate(cpid, rate);
    else if (zfpmode == H5Z_ZFP_MODE_PRECISION)
        H5Pset_zfp_precision(cpid, prec);
    else if (zfpmode == H5Z_ZFP_MODE_ACCURACY)
        H5Pset_zfp_accuracy(cpid, acc);
    else if (zfpmode == H5Z_ZFP_MODE_EXPERT)
        H5Pset_zfp_expert(cpid, minbits, maxbits, maxprec, minexp);

#endif

    return cpid;
}


int main(int argc, char **argv)
{
    int i;

    /* filename variables */
    char *ifile = (char *) calloc(NAME_LEN,sizeof(char));
    char *ids   = (char *) calloc(NAME_LEN,sizeof(char));
    char *ofile = (char *) calloc(NAME_LEN,sizeof(char));

    /* sinusoid data generation variables */
    hsize_t npoints = 1024;
    double noise = 0.001;
    double amp = 17.7;
    int doint = 0;
    int highd = 0;
    int help = 0;

    /* compression parameters (defaults taken from ZFP header) */
    int zfpmode = H5Z_ZFP_MODE_ACCURACY;
    double rate = 4;
    double acc = 0;
    uint prec = 11;
    uint minbits = 0;
    uint maxbits = 4171;
    uint maxprec = 64;
    int minexp = -1074;
    int *ibuf = 0;
    double *buf = 0;

    /* HDF5 related variables */
    hsize_t chunk = 256;
    hid_t fid, dsid, idsid, sid, cpid;

    /* file arguments */
    strcpy(ofile, "test_zfp.h5");
    HANDLE_ARG(ifile,strndup(argv[i]+len2,NAME_LEN), "\"%s\"",set input filename);
    /*HANDLE_ARG(ids,strndup(argv[i]+len2,NAME_LEN), "\"%s\"",set input datast name);*/
    HANDLE_ARG(ofile,strndup(argv[i]+len2,NAME_LEN), "\"%s\"",set output filename);

    /* data generation arguments */
    HANDLE_ARG(npoints,(hsize_t) strtol(argv[i]+len2,0,10), "%llu",set number of points for generated dataset);
    HANDLE_ARG(noise,(double) strtod(argv[i]+len2,0),"%g",set amount of random noise in generated dataset);
    HANDLE_ARG(amp,(double) strtod(argv[i]+len2,0),"%g",set amplitude of sinusoid in generated dataset);
    HANDLE_ARG(doint,(int) strtol(argv[i]+len2,0,10),"%d",also do integer data);
    HANDLE_ARG(highd,(int) strtol(argv[i]+len2,0,10),"%d",run high-dimensional (>3D) case);

    /* HDF5 chunking and ZFP filter arguments */
    HANDLE_ARG(chunk,(hsize_t) strtol(argv[i]+len2,0,10), "%llu",set chunk size for dataset);
    HANDLE_ARG(zfpmode,(int) strtol(argv[i]+len2,0,10),"%d",set zfp mode (1=rate,2=prec,3=acc,4=expert)); 
    HANDLE_ARG(rate,(double) strtod(argv[i]+len2,0),"%g",set rate for rate mode of filter);
    HANDLE_ARG(acc,(double) strtod(argv[i]+len2,0),"%g",set accuracy for accuracy mode of filter);
    HANDLE_ARG(prec,(uint) strtol(argv[i]+len2,0,10),"%u",set precision for precision mode of zfp filter);
    HANDLE_ARG(minbits,(uint) strtol(argv[i]+len2,0,10),"%u",set minbits for expert mode of zfp filter);
    HANDLE_ARG(maxbits,(uint) strtol(argv[i]+len2,0,10),"%u",set maxbits for expert mode of zfp filter);
    HANDLE_ARG(maxprec,(uint) strtol(argv[i]+len2,0,10),"%u",set maxprec for expert mode of zfp filter);
    HANDLE_ARG(minexp,(int) strtol(argv[i]+len2,0,10),"%d",set minexp for expert mode of zfp filter);
    cpid = setup_filter(1, &chunk, zfpmode, rate, acc, prec, minbits, maxbits, maxprec, minexp);
    /* Put this after setup_filter to permit printing of otherwise hard to 
       construct cd_values to facilitate manual invokation of h5repack */
    HANDLE_ARG(help,(int)strtol(argv[i]+len2,0,10),"%d",this help message); /* must be last for help to work */

    /* create double data to write if we're not reading from an existing file */
    if (ifile[0] == '\0')
        gen_data((size_t) npoints, noise, amp, (void**)&buf, TYPDBL);
    else
        read_data(ifile, (size_t) npoints, &buf);

    /* create integer data to write */
    if (doint)
        gen_data((size_t) npoints, noise*100, amp*1000000, (void**)&ibuf, TYPINT);

    /* create HDF5 file */
    if (0 > (fid = H5Fcreate(ofile, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT))) ERROR(H5Fcreate);

    /* setup the 1D data space */
    if (0 > (sid = H5Screate_simple(1, &npoints, 0))) ERROR(H5Screate_simple);

    /* write the data WITHOUT compression */
    if (0 > (dsid = H5Dcreate(fid, "original", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT))) ERROR(H5Dcreate);
    if (0 > H5Dwrite(dsid, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf)) ERROR(H5Dwrite);
    if (0 > H5Dclose(dsid)) ERROR(H5Dclose);
    if (doint)
    {
        if (0 > (idsid = H5Dcreate(fid, "int_original", H5T_NATIVE_INT, sid, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT))) ERROR(H5Dcreate);
        if (0 > H5Dwrite(idsid, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ibuf)) ERROR(H5Dwrite);
        if (0 > H5Dclose(idsid)) ERROR(H5Dclose);
    }

    /* write the data with requested compression */
    if (0 > (dsid = H5Dcreate(fid, "compressed", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, cpid, H5P_DEFAULT))) ERROR(H5Dcreate);
    if (0 > H5Dwrite(dsid, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf)) ERROR(H5Dwrite);
    if (0 > H5Dclose(dsid)) ERROR(H5Dclose);
    if (doint)
    {
        if (0 > (idsid = H5Dcreate(fid, "int_compressed", H5T_NATIVE_INT, sid, H5P_DEFAULT, cpid, H5P_DEFAULT))) ERROR(H5Dcreate);
        if (0 > H5Dwrite(idsid, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ibuf)) ERROR(H5Dwrite);
        if (0 > H5Dclose(idsid)) ERROR(H5Dclose);
    }

    /* clean up from simple tests */
    if (0 > H5Sclose(sid)) ERROR(H5Sclose);
    if (0 > H5Pclose(cpid)) ERROR(H5Pclose);
    free(buf);
    if (ibuf) free(ibuf);

    /* Test high dimensional (>3D) array */
    if (highd)
    {
        int fd, dims[] = {128,128,16,32}, ucdims[]={1,3};
        hsize_t hdims[] = {128,128,16,32};
        hsize_t hchunk[] = {1,128,1,32};

        buf = gen_random_correlated_array(TYPDBL, 4, dims, 2, ucdims);

        cpid = setup_filter(4, hchunk, zfpmode, rate, acc, prec, minbits, maxbits, maxprec, minexp);

        if (0 > (sid = H5Screate_simple(4, hdims, 0))) ERROR(H5Screate_simple);

        /* write the data WITHOUT compression */
        if (0 > (dsid = H5Dcreate(fid, "highD_original", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT))) ERROR(H5Dcreate);
        if (0 > H5Dwrite(dsid, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf)) ERROR(H5Dwrite);
        if (0 > H5Dclose(dsid)) ERROR(H5Dclose);

        /* write the data with compression */
        if (0 > (dsid = H5Dcreate(fid, "highD_compressed", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, cpid, H5P_DEFAULT))) ERROR(H5Dcreate);
        if (0 > H5Dwrite(dsid, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf)) ERROR(H5Dwrite);
        if (0 > H5Dclose(dsid)) ERROR(H5Dclose);

        /* clean up from high dimensional test */
        if (0 > H5Sclose(sid)) ERROR(H5Sclose);
        if (0 > H5Pclose(cpid)) ERROR(H5Pclose);
        free(buf);
    }

    if (0 > H5Fclose(fid)) ERROR(H5Fclose);

    free(ifile);
    free(ofile);
    free(ids);

#ifndef H5Z_ZFP_USE_PLUGIN
    /* When filter is used as a library, we need to finalize it */
    H5Z_zfp_finalize();
#endif

    H5close();

    return 0;
}
