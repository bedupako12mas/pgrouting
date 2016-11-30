/*PGR-MIT*****************************************************************

 * Traveling Sales Problem solver for pgRouting and PostgreSQL
 *
 * Copyright 2013, Stephen Woodbridge, iMaptools.com
 * This program is released under an MIT-X license.
 *

------
MIT/X license

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:


The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.


THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

********************************************************************PGR-MIT*/

/*
 * This code is deprecated
 */
#include "./../../common/src/postgres_connection.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "./tsp.h"
#include <math.h>


PGDLLEXPORT Datum tsp_matrix(PG_FUNCTION_ARGS);

#undef DEBUG
#include "../../common/src/debug_macro.h"

#if 0
#ifndef INFINITY
#ifndef _MSC_VER
#define INFINITY (1.0/0.0)
#else  // _MSC_VER
#define INFINITY (DBL_MAX + DBL_MAX);
#endif  // _MSC_VER
#endif
#endif

// The number of tuples to fetch from the SPI cursor at each iteration
#define TUPLIMIT 1000


static double *get_pgarray(int *num, ArrayType *input) {
    int         ndims, *dims;  // , *lbs;
    bool       *nulls;
    Oid         i_eltype;
    int16       i_typlen;
    bool        i_typbyval;
    char        i_typalign;
    Datum      *i_data;
    int         i, n;
    double      *data;

    /* get input array element type */
    i_eltype = ARR_ELEMTYPE(input);
    get_typlenbyvalalign(i_eltype, &i_typlen, &i_typbyval, &i_typalign);


    /* validate input data type */
    switch (i_eltype) {
    case INT2OID:
    case INT4OID:
    case FLOAT4OID:
    case FLOAT8OID:
            break;
    default:
            elog(ERROR, "Invalid input data type");
            break;
    }

    /* get various pieces of data from the input array */
    ndims = ARR_NDIM(input);
    dims = ARR_DIMS(input);
    // lbs = ARR_LBOUND(input);

    if (ndims != 2 || dims[0] != dims[1]) {
        elog(ERROR, "Error: matrix[num][num] in its definition.");
    }

    /* get src data */
    deconstruct_array(input, i_eltype, i_typlen, i_typbyval, i_typalign,
&i_data, &nulls, &n);

    PGR_DBG("get_pgarray: ndims=%d, n=%d", ndims, n);

#ifdef DEBUG
    for (i = 0; i < ndims; i++) {
        // PGR_DBG("   dims[%d]=%d, lbs[%d]=%d", i, dims[i], i, lbs[i]);
        PGR_DBG("   dims[%d]=%d", i, dims[i]);
    }
#endif

    /* construct a C array */
    data = (double *) palloc((size_t)(n) * sizeof(double));
    if (!data) {
        elog(ERROR, "Error: Out of memory!");
    }

    for (i = 0; i < n; i++) {
        if (nulls[i]) {
            data[i] = (double) 0;
        } else {
            switch (i_eltype) {
                case INT2OID:
                    data[i] = (double) DatumGetInt16(i_data[i]);
                    break;
                case INT4OID:
                    data[i] = (double) DatumGetInt32(i_data[i]);
                    break;
                case FLOAT4OID:
                    data[i] = (double) DatumGetFloat4(i_data[i]);
                    break;
                case FLOAT8OID:
                    data[i] = (double) DatumGetFloat8(i_data[i]);
                    break;
            }
            /* we assume negative values are INFINTY */
            /********************************************************
               TODO: based on trying to add an endpt it is likely
               that this will not work and you will get and error in
               findEulerianPath
            **********************************************************/
            if (data[i] < 0) {
                data[i] = (double) 0;
                nulls[i] = true;
            }
        }
        PGR_DBG("    data[%d]=%.4f", i, data[i]);
    }

    pfree(nulls);
    pfree(i_data);

    *num = dims[0];

    return data;
}


// macro to store distance values as matrix[num][num]
#define D(i, j) matrix[(i) * num + j]

static int solve_tsp(double *matrix, int num, int start, int end, int **results) {
    int ret;
    int i;
    int *ids;
    double fit;
    char *err_msg = NULL;

    PGR_DBG("In solve_tsp: num: %d, start: %d, end: %d", num, start, end);

    if (num < 4)
        elog(ERROR, "Error TSP requires four or more locations to optimize. Only %d were supplied.", num);

    if (start < 0 || start >= num)
        elog(ERROR, "Error start must be in the range of 0 <= start(%d) < num(%d).", start, num);

    if (end >= num)
        elog(ERROR, "Error end must be in the range of 0 <= end(%d) < num(%d).", end, num);

    /* if start and end are the same this is the same as setting end = -1 */
    if (start == end)
        end = -1;

    /*
       fix up matrix id we have an end point
       basically set D(start,end)=INFINITY and D(end,start)=0.0
    */
    if (end >= 0) {
        PGR_DBG("Updating start end costs");
        D(start, end) = 0.0;
        D(end, start) = 0.0;
    }

    PGR_DBG("Alloc ids");

    ids = (int *) malloc((size_t)(num) * sizeof(int));
    if (!ids) {
        elog(ERROR, "Error: Out of memory (solve_tsp)");
    }

    for (i = 0; i < num; i++) {
        ids[i] = i;
    }

    PGR_DBG("Calling find_tsp_solution");

// int find_tsp_solution(int num, double *dist, int *p_ids, int source, double *fit, char* err_msg);
    ret = find_tsp_solution(num, matrix, ids, start, end, &fit, err_msg);
    if (ret < 0) {
        elog(ERROR, "Error solving TSP, %s", err_msg);
    }

    PGR_DBG("TSP solved, Score: %f", fit);

    *results = ids;
    return ret;
}

/*
 * select seq, id from pgr_tsp(matrix float8[][], start int,
 *                             OUT seq int, OUT id int);
*/

PG_FUNCTION_INFO_V1(tsp_matrix);
PGDLLEXPORT Datum
tsp_matrix(PG_FUNCTION_ARGS) {
    FuncCallContext     *funcctx;
    TupleDesc            tuple_desc;
    // AttInMetadata       *attinmeta;

    double               *matrix;
    int                 *tsp_res;
    int                  num;

    /* stuff done only on the first call of the function */
    if (SRF_IS_FIRSTCALL()) {
        MemoryContext   oldcontext;
        // int path_count;
        int ret = -1;

        /* create a function context for cross-call persistence */
        funcctx = SRF_FIRSTCALL_INIT();

        /* switch to memory context appropriate for multiple function calls */
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        matrix = get_pgarray(&num, PG_GETARG_ARRAYTYPE_P(0));

        ret = solve_tsp(matrix, num,
                        PG_GETARG_INT32(1),  // start index
                        PG_GETARG_INT32(2),  // end  index
                        &tsp_res);

        pfree(matrix);
        if (ret < 0) {
            elog(ERROR, "Error, failed to solve TSP.");
        }

#if PGSQL_VERSION > 95
        funcctx->max_calls = (size_t)num;
#else
        funcctx->max_calls = (uint32_t)num;
#endif

        funcctx->user_fctx = tsp_res;

        /* Build a tuple descriptor for our result type */
        if (get_call_result_type(fcinfo, NULL, &tuple_desc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context "
                            "that cannot accept type record")));

        funcctx->tuple_desc = BlessTupleDesc(tuple_desc);

        /*
         * generate attribute metadata needed later to produce tuples from raw
         * C strings
         */
        // attinmeta = TupleDescGetAttInMetadata(tuple_desc);
        // funcctx->attinmeta = attinmeta;

        MemoryContextSwitchTo(oldcontext);
    }

    /* stuff done on every call of the function */
    funcctx = SRF_PERCALL_SETUP();

    tuple_desc = funcctx->tuple_desc;
    tsp_res    = funcctx->user_fctx;

    PGR_DBG("Trying to allocate some memory");

    if (funcctx->call_cntr < funcctx->max_calls) {   /* do when there is more left to send */
        HeapTuple    tuple;
        Datum        result;
        Datum *values;
        bool* nulls;

        values = palloc(2 * sizeof(Datum));
        nulls = palloc(2 * sizeof(bool));

        values[0] = Int32GetDatum(funcctx->call_cntr);
        nulls[0] = false;
        values[1] = Int32GetDatum(tsp_res[funcctx->call_cntr]);
        nulls[1] = false;


        PGR_DBG("Heap making");

        tuple = heap_form_tuple(tuple_desc, values, nulls);

        PGR_DBG("Datum making");

        /* make the tuple into a datum */
        result = HeapTupleGetDatum(tuple);

        PGR_DBG("Trying to free some memory");

        /* clean up (this is not really necessary) */
        pfree(values);
        pfree(nulls);


        SRF_RETURN_NEXT(funcctx, result);
    } else {   /* do when there is no more left */
        PGR_DBG("Freeing tsp_res");
        free(tsp_res);

        PGR_DBG("Ending function");
        SRF_RETURN_DONE(funcctx);
    }
}
