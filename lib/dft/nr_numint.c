/*
 * Author: Qiming Sun <osirpt.sun@gmail.com>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cint.h"
#include "gto/grid_ao_drv.h"
#include "np_helper/np_helper.h"
#include "vhf/fblas.h"
#include <assert.h>

#define BOXSIZE         40

static int empty_blocks(char *empty, unsigned char *non0table, int *shls_slice,
                        int *ao_loc)
{
        const int sh0 = shls_slice[0];
        const int sh1 = shls_slice[1];

        int bas_id;
        int box_id = 0;
        int bound = BOXSIZE;
        empty[box_id] = 1;
        for (bas_id = sh0; bas_id < sh1; bas_id++) {
                empty[box_id] &= !non0table[bas_id];
                if (ao_loc[bas_id] == bound) {
                        box_id++;
                        bound += BOXSIZE;
                        empty[box_id] = 1;
                } else if (ao_loc[bas_id] > bound) {
                        box_id++;
                        bound += BOXSIZE;
                        empty[box_id] = !non0table[bas_id];
                }
        }
        int nbox = box_id + 1;
        return nbox;
}

static void dot_ao_dm(double *vm, double *ao, double *dm,
                      int nao, int nocc, int ngrids, int bgrids,
                      unsigned char *non0table, int *shls_slice, int *ao_loc)
{
        char empty[nao/BOXSIZE+1];
        int nbox = empty_blocks(empty, non0table, shls_slice, ao_loc);

        const char TRANS_T = 'T';
        const char TRANS_N = 'N';
        const double D1 = 1;
        int box_id, bas_id, b0, blen, i, j;
        double beta = 0;

        for (box_id = 0; box_id < nbox; box_id++) {
                if (!empty[box_id]) {
                        b0 = box_id * BOXSIZE;
                        blen = MIN(nao-b0, BOXSIZE);
                        dgemm_(&TRANS_N, &TRANS_T, &bgrids, &nocc, &blen,
                               &D1, ao+b0*ngrids, &ngrids, dm+b0*nocc, &nocc,
                               &beta, vm, &ngrids);
                        beta = 1.0;
                }
        }
        if (beta == 0) { // all empty
                for (i = 0; i < nocc; i++) {
                        for (j = 0; j < bgrids; j++) {
                                vm[i*ngrids+j] = 0;
                        }
                }
        }
}


/* vm[nocc,ngrids] = ao[i,ngrids] * dm[i,nocc] */
void VXCdot_ao_dm(double *vm, double *ao, double *dm,
                  int nao, int nocc, int ngrids, int nbas,
                  unsigned char *non0table, int *shls_slice, int *ao_loc)
{
        const int nblk = (ngrids+BLKSIZE-1) / BLKSIZE;

#pragma omp parallel default(none) \
        shared(vm, ao, dm, nao, nocc, ngrids, nbas, \
               non0table, shls_slice, ao_loc)
{
        int ip, ib;
#pragma omp for nowait schedule(static)
        for (ib = 0; ib < nblk; ib++) {
                ip = ib * BLKSIZE;
                dot_ao_dm(vm+ip, ao+ip, dm,
                          nao, nocc, ngrids, MIN(ngrids-ip, BLKSIZE),
                          non0table+ib*nbas, shls_slice, ao_loc);
        }
}
}



/* vv[n,m] = ao1[n,ngrids] * ao2[m,ngrids] */
static void dot_ao_ao(double *vv, double *ao1, double *ao2,
                      int nao, int ngrids, int bgrids, int hermi,
                      unsigned char *non0table, int *shls_slice, int *ao_loc)
{
        char empty[nao/BOXSIZE+1];
        int nbox = empty_blocks(empty, non0table, shls_slice, ao_loc);

        const char TRANS_T = 'T';
        const char TRANS_N = 'N';
        const double D1 = 1;
        int ib, jb, b0i, b0j, leni, lenj;
        int j1 = nbox;

        for (ib = 0; ib < nbox; ib++) {
                if (!empty[ib]) {
                        b0i = ib * BOXSIZE;
                        leni = MIN(nao-b0i, BOXSIZE);
                        if (hermi) {
                                j1 = ib + 1;
                        }
                        for (jb = 0; jb < j1; jb++) {
                                if (!empty[jb]) {
                                        b0j = jb * BOXSIZE;
                                        lenj = MIN(nao-b0j, BOXSIZE);
                                        dgemm_(&TRANS_T, &TRANS_N,
                                               &lenj, &leni, &bgrids, &D1,
                                               ao2+b0j*ngrids, &ngrids,
                                               ao1+b0i*ngrids, &ngrids,
                                               &D1, vv+b0i*nao+b0j, &nao);
                                }
                        }
                }
        }
}


/* vv[nao,nao] = ao1[i,nao] * ao2[i,nao] */
void VXCdot_ao_ao(double *vv, double *ao1, double *ao2,
                  int nao, int ngrids, int nbas, int hermi,
                  unsigned char *non0table, int *shls_slice, int *ao_loc)
{
        const int nblk = (ngrids+BLKSIZE-1) / BLKSIZE;
        memset(vv, 0, sizeof(double) * nao * nao);

#pragma omp parallel default(none) \
        shared(vv, ao1, ao2, nao, ngrids, nbas, hermi, \
               non0table, shls_slice, ao_loc)
{
        int ip, ib;
        double *v_priv = calloc(nao*nao, sizeof(double));
#pragma omp for nowait schedule(static)
        for (ib = 0; ib < nblk; ib++) {
                ip = ib * BLKSIZE;
                dot_ao_ao(v_priv, ao1+ip, ao2+ip,
                          nao, ngrids, MIN(ngrids-ip, BLKSIZE), hermi,
                          non0table+ib*nbas, shls_slice, ao_loc);
        }
#pragma omp critical
        {
                for (ip = 0; ip < nao*nao; ip++) {
                        vv[ip] += v_priv[ip];
                }
        }
        free(v_priv);
}
        if (hermi != 0) {
                NPdsymm_triu(nao, vv, hermi);
        }
}
