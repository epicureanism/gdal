/*************************************************************************
 * File :    postgisrasterdataset.cpp
 * Project:  PostGIS Raster driver
 * Purpose:  GDAL Dataset implementation for PostGIS Raster driver
 * Author:   Jorge Arevalo, jorge.arevalo@deimos-space.com
 *
 * Last changes:
 * $Id:$
 *
 ************************************************************************
 * Copyright (c) 2009 - 2011, Jorge Arevalo, jorge.arevalo@deimos-space.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ************************************************************************/

#include "postgisraster.h"
#include <stdlib.h>
#include "ogr_api.h"
#include "ogr_geometry.h"
#include "gdal.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal_priv.h"
#include <math.h>
#include "cpl_error.h"
#include "ogr_core.h"

#ifdef _WIN32
#define rint(x) floor((x) + 0.5)
#endif

CPL_C_START
void GDALRegister_PostGISRaster(void);

CPL_C_END



/************************
 * \brief Constructor
 ************************/
PostGISRasterDataset::PostGISRasterDataset() {
    papszSubdatasets = NULL;
    nSrid = -1;
    poConn = NULL;
    bRegisteredInRasterColumns = false;
    pszSchema = NULL;
    pszTable = NULL;
    pszColumn = NULL;
    pszWhere = NULL;
    pszProjection = NULL;
    nMode = NO_MODE;
    poDriver = NULL;
    nBlockXSize = 0;
    nBlockYSize = 0;
    adfGeoTransform[0] = 0.0; /* X Origin (top left corner) */
    adfGeoTransform[1] = 1.0; /* X Pixel size */
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = 0.0; /* Y Origin (top left corner) */
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = 1.0; /* Y Pixel Size */
    bBlocksCached = false;
    bRegularBlocking = false;
    bAllTilesSnapToSameGrid = false;

    /**
     * TODO: Parametrize bAllTilesSnapToSameGrid. It controls if all the
     * raster rows, in ONE_RASTER_PER_TABLE mode, must be checked to test if
     * they snap to the same grid and have the same srid. It can be the user
     * decission, if he/she's sure all the rows pass the test and want more
     * speed.
     **/

}

/************************
 * \brief Constructor
 ************************/
PostGISRasterDataset::~PostGISRasterDataset() {

    if (pszSchema)
        CPLFree(pszSchema);
    if (pszTable)
        CPLFree(pszTable);
    if (pszColumn)
        CPLFree(pszColumn);
    if (pszWhere)
        CPLFree(pszWhere);
    if (pszProjection)
        CPLFree(pszProjection);

    if (papszSubdatasets)
        CSLDestroy(papszSubdatasets);
}

/**************************************************************
 * \brief Replace the single quotes by " in the input string
 *
 * Needed before tokenize function
 *************************************************************/
static
char * ReplaceSingleQuotes(const char * pszInput, int nLength) {
    int i;
    char* pszOutput = NULL;

    if (nLength == -1)
        nLength = strlen(pszInput);

    pszOutput = (char*) CPLCalloc(nLength + 1, sizeof (char));

    for (i = 0; i < nLength; i++) {
        if (pszInput[i] == '\'')
            pszOutput[i] = '"';
        else
            pszOutput[i] = pszInput[i];

    }

    return pszOutput;
}

/**************************************************************
 * \brief Replace the quotes by single quotes in the input string
 *
 * Needed in the 'where' part of the input string
 *************************************************************/
static
char * ReplaceQuotes(const char * pszInput, int nLength) {
    int i;
    char * pszOutput = NULL;

    if (nLength == -1)
        nLength = strlen(pszInput);

    pszOutput = (char*) CPLCalloc(nLength + 1, sizeof (char));

    for (i = 0; i < nLength; i++) {
        if (pszInput[i] == '"')
            pszOutput[i] = '\'';
        else
            pszOutput[i] = pszInput[i];
    }

    return pszOutput;
}

/*****************************************************************************
 * \brief Split connection string into user, password, host, database...
 *
 * The parameters separated by spaces are return as a list of strings. The
 * function accepts all the PostgreSQL recognized parameter key words.
 *
 * The returned list must be freed with CSLDestroy when no longer needed
 *
 *****************************************************************************/
static
char** ParseConnectionString(const char * pszConnectionString) {
    char * pszEscapedConnectionString = NULL;

    /* Escape string following SQL scheme */
    pszEscapedConnectionString = ReplaceSingleQuotes(pszConnectionString, -1);

    /* Avoid PG: part */
    char* pszStartPos = (char*) strstr(pszEscapedConnectionString, ":") + 1;

    /* Tokenize */
    char** papszParams = CSLTokenizeString2(pszStartPos, " ",
            CSLT_HONOURSTRINGS);

    /* Free */
    CPLFree(pszEscapedConnectionString);

    return papszParams;

}

/**************************************************************************
 * \brief Look for raster tables in database and store them as subdatasets
 *
 * If no table is provided in connection string, the driver looks for the
 * existent raster tables in the schema given as argument. This argument,
 * however, is optional. If a NULL value is provided, the driver looks for
 * all raster tables in all schemas of the user-provided database.
 *
 * NOTE: Permissions are managed by libpq. The driver only returns an error
 * if an error is returned when trying to access to tables not allowed to
 * the current user.
 **************************************************************************/
GBool PostGISRasterDataset::BrowseDatabase(const char* pszCurrentSchema,
        char* pszValidConnectionString) {
    /* Be careful! These 3 vars override the class ones! */
    char* pszSchema = NULL;
    char* pszTable = NULL;
    char* pszColumn = NULL;
    int i = 0;
    int nTuples = 0;
    PGresult * poResult = NULL;
    CPLString osCommand;


    /*************************************************************
     * Fetch all the raster tables and store them as subdatasets
     *************************************************************/
    if (pszCurrentSchema == NULL) {
        osCommand.Printf("select pg_namespace.nspname as schema, pg_class.relname as \
					table, pg_attribute.attname as column from pg_class, \
					pg_namespace,pg_attribute, pg_type where \
					pg_class.relnamespace = pg_namespace.oid and pg_class.oid = \
					pg_attribute.attrelid and pg_attribute.atttypid = pg_type.oid \
					and pg_type.typname = 'raster'");

        poResult = PQexec(poConn, osCommand.c_str());
        if (
                poResult == NULL ||
                PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                PQntuples(poResult) <= 0
                ) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Error browsing database for PostGIS Raster tables: %s", PQerrorMessage(poConn));
            if (poResult != NULL)
                PQclear(poResult);

            return false;
        }


        nTuples = PQntuples(poResult);
        for (i = 0; i < nTuples; i++) {
            pszSchema = PQgetvalue(poResult, i, 0);
            pszTable = PQgetvalue(poResult, i, 1);
            pszColumn = PQgetvalue(poResult, i, 2);

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_NAME", (i + 1)),
                    CPLSPrintf("PG:%s schema=%s table=%s column=%s",
                    pszValidConnectionString, pszSchema, pszTable, pszColumn));

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_DESC", (i + 1)),
                    CPLSPrintf("PostGIS Raster table at %s.%s (%s)", pszSchema, pszTable, pszColumn));
        }

        PQclear(poResult);

    }
        /**********************************************************************
         * Fetch all the schema's raster tables and store them as subdatasets
         **********************************************************************/
    else {
        osCommand.Printf("select pg_class.relname as table, pg_attribute.attname \
			 as column from pg_class, pg_namespace,pg_attribute, pg_type where \
			 pg_class.relnamespace = pg_namespace.oid and pg_class.oid = \
			 pg_attribute.attrelid and pg_attribute.atttypid = pg_type.oid \
			 and pg_type.typname = 'raster' and pg_namespace.nspname = '%s'",
                pszCurrentSchema);

        poResult = PQexec(poConn, osCommand.c_str());
        if (
                poResult == NULL ||
                PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                PQntuples(poResult) <= 0
                ) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Error browsing database for PostGIS Raster tables: %s", PQerrorMessage(poConn));
            if (poResult != NULL)
                PQclear(poResult);

            return false;
        }


        nTuples = PQntuples(poResult);
        for (i = 0; i < nTuples; i++) {
            pszTable = PQgetvalue(poResult, i, 0);
            pszColumn = PQgetvalue(poResult, i, 1);

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_NAME", (i + 1)),
                    CPLSPrintf("PG:%s schema=%s table=%s column=%s",
                    pszValidConnectionString, pszCurrentSchema, pszTable, pszColumn));

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_DESC", (i + 1)),
                    CPLSPrintf("PostGIS Raster table at %s.%s (%s)", pszCurrentSchema,
                    pszTable, pszColumn));
        }

        PQclear(poResult);
    }

    return true;
}

/*************************************************************************
 * \brief Set the general raster properties.
 *
 * We must distinguish between tiled and untiled raster coverages. In
 * PostGIS Raster, there's no real difference between 'tile' and 'raster'.
 * There's only 'raster objects'. Each record of a raster table is a
 * raster object, and has its own georeference information, whether if
 * the record is a tile of a bigger raster coverage or is a complete
 * raster. So, <b>there's no a way of knowing if the rows of a raster
 * table are related or not</b>. It's user's responsibility. The only
 * thing driver can do is to suppose all the rows of a table are from
 * the same raster coverage if the user has queried for one table, without
 * specifying a where clause.
 *
 * The user is responsible to ensure that the raster layer meets the minimum
 * topological requirements for analysis. The ideal case is when all the raster
 * tiles of a continuous layer are the same size, snap to the same grid and do
 * not overlap.
 *
 * So, when we query for a raster table, we have 3 different cases:
 *	- If the result is only one row, we can gather the raster properties
 *	  from the returned object, regardless is a tile or a whole raster
 *	- If the result are several rows of a table, and the working mode is
 *    ONE_RASTER_PER_TABLE, we assume all the rows are from the same raster
 *    coverage. The rows are ordered by upper left y, upper left x, growing way,
 *    and we can get raster size from the first and last elements.
 *  - If the result are several rows of a table, and the working mode is
 *    ONE_RASTER_PER_ROW, we assume each row is a different raster object,
 *    and is reported as a subdataset. If you want only one of the raster rows,
 *    you must specify a where clause to restrict the number of rows returned.
 **************************************************************************/
GBool PostGISRasterDataset::SetRasterProperties
    (const char * pszValidConnectionString)
{
    PGresult* poResult = NULL;
    CPLString osCommand;
    int i = 0;
    int nTuples = 0;
    int nRasterID = 0;
    GBool bRetValue = false;
    OGRSpatialReference * poSR = NULL;
    OGRGeometry* poGeom = NULL;
    OGRErr OgrErr = OGRERR_NONE;
    OGREnvelope* poE = NULL;
    char* pszExtent;
    char* pszProjectionRef;
    char* pszIdColumn = NULL;
    /* Incorporated variables from old SetRasterBand method */
    GBool bSignedByte = false;
    int nBitDepth = 8;
    char* pszDataType = NULL;
    int iBand = 0;
    double dfNodata = 0.0;
    GDALDataType hDataType = GDT_Byte;
    GBool bIsOffline = false;
    GBool bHasNoDataValue = false;

    /* Execute the query to fetch raster data from db */
    if (pszWhere == NULL) {
        osCommand.Printf("select count(*) from %s.%s", 
            pszSchema, pszTable);
    } else {
        osCommand.Printf("select count(*) from %s.%s where %s", 
            pszSchema, pszTable, pszWhere);
    }

    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
            "Query: %s", osCommand.c_str());
    poResult = PQexec(poConn, osCommand.c_str());
    if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            PQntuples(poResult) <= 0
            ) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Error browsing database for PostGIS Raster properties");

        if (poResult != NULL)
            PQclear(poResult);


        return false;
    }

    nTuples = atoi(PQgetvalue(poResult, 0, 0));

    PQclear(poResult);

    /******************************************
     * Easier case. Only one raster to fetch
     ******************************************/
    if (nTuples == 1 || ONE_RASTER_PER_TABLE) {
        if (pszWhere == NULL) {
            osCommand.Printf(
                "select st_xmin(geom) xmin, st_ymax(geom) ymin, srid, nbband "
                "from (select st_srid(%s) srid, st_extent(%s::geometry) geom, "
                "max(ST_NumBands(rast)) nbband from %s.%s "
                "group by srid) foo",
                pszColumn, pszColumn, pszSchema, pszTable);
        }
        else {
            osCommand.Printf(
                "select st_xmin(geom) xmin, st_ymax(geom) ymin, srid, nbband "
                "from (select st_srid(%s) srid, st_extent(%s::geometry) geom, "
                "max(ST_NumBands(rast)) nbband from %s.%s where %s "
                "group by srid) foo",
                pszColumn, pszColumn, pszSchema, pszTable, pszWhere);
        }

        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
            "Query: %s", osCommand.c_str());
        poResult = PQexec(poConn, osCommand.c_str());
        if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            PQntuples(poResult) <= 0
            ) {
            CPLError(CE_Failure, CPLE_AppDefined,
                "Error retrieving aggregate raster metadata");

            if (poResult != NULL)
                PQclear(poResult);

            return false;
        }

        adfGeoTransform[0] = atof(PQgetvalue(poResult, 0, 0)); //upperleft x
        adfGeoTransform[3] = atof(PQgetvalue(poResult, 0, 1)); //upperleft y
        nSrid = atoi(PQgetvalue(poResult, 0, 2));
        nBands = atoi(PQgetvalue(poResult, 0, 3));

        if (pszWhere == NULL) {
            osCommand.Printf(
                "select st_scalex(%s), st_scaley(%s), st_skewx(%s), "
                "st_skewy(%s), st_width(%s), st_height(%s) from %s.%s limit 1",
                pszColumn, pszColumn, pszColumn, pszColumn,
                pszColumn, pszColumn, pszSchema, pszTable);
        }
        else {
            osCommand.Printf(
                "select st_scalex(%s), st_scaley(%s), st_skewx(%s), "
                "st_skewy(%s), st_width(%s), st_height(%s) from %s.%s "
                "where %s limit 1",
                pszColumn, pszColumn, pszColumn, pszColumn,
                pszColumn, pszColumn, pszSchema, pszTable, pszWhere);
        }

        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
            "Query: %s", osCommand.c_str());
        poResult = PQexec(poConn, osCommand.c_str());
        if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            PQntuples(poResult) <= 0
            ) {
            CPLError(CE_Failure, CPLE_AppDefined,
                "Error retrieving first raster metadata");

            if (poResult != NULL)
                PQclear(poResult);

            return false;
        }

        adfGeoTransform[1] = atof(PQgetvalue(poResult, 0, 0)); //pixelsize x
        adfGeoTransform[5] = atof(PQgetvalue(poResult, 0, 1)); //pixelsize y
        adfGeoTransform[2] = atof(PQgetvalue(poResult, 0, 2)); //skew x
        adfGeoTransform[4] = atof(PQgetvalue(poResult, 0, 3)); //skew y

        nBlockXSize = atoi(PQgetvalue(poResult, 0, 4));
        nBlockYSize = atoi(PQgetvalue(poResult, 0, 5));

        PQclear(poResult);

        /**
         * Not tiled dataset: The whole raster.
         * TODO: 'invent' a good block size.
         */
        nRasterXSize = nBlockXSize;
        nRasterYSize = nBlockYSize;

        bRetValue = true;
    }
    
    if (nTuples > 1 ) {
        switch (nMode) {
                /**
                 * Each row is a different raster. Create subdatasets, one per row
                 **/

            case ONE_RASTER_PER_ROW:
                {
                /* Determine the primary key/unique column on the table */
                osCommand.Printf("select d.attname from pg_catalog.pg_constraint as a "
                    "join pg_catalog.pg_indexes as b on a.conname = b.indexname "
                    "join pg_catalog.pg_class as c on c.relname = b.tablename "
                    "join pg_catalog.pg_attribute as d on c.relfilenode = d.attrelid "
                    "where b.schemaname = '%s' and b.tablename = '%s' and "
                    "d.attnum = a.conkey[1] and a.contype in ('p', 'u')", 
                    pszSchema, pszTable);

                CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
                        "Query: %s", osCommand.c_str());
                poResult = PQexec(poConn, osCommand.c_str());
                if (poResult == NULL ||
                    PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                    PQntuples(poResult) <= 0 ) {

                    PQclear(poResult);

                    /*
                       Maybe there is no primary key or unique constraint;
                       a sequence will also suffice; get the first one
                     */

                    osCommand.Printf("select cols.column_name from information_schema."
                        "columns as cols join information_schema.sequences as seqs on cols."
                        "column_default like '%%'||seqs.sequence_name||'%%' where cols."
                        "table_schema = '%s' and cols.table_name = '%s'", pszSchema, 
                        pszTable);

                    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
                            "Query: %s", osCommand.c_str());
                    poResult = PQexec(poConn, osCommand.c_str());

                    if (poResult == NULL || 
                        PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                        PQntuples(poResult) <= 0) {

                        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
                            "Could not find a primary key or unique column on the specified table; using UpperLeftX and UpperLeftY.");

                        /* 
                            If no primary key or unique column found, 
                            fall back to raster upperleft x&y
                         */
                    }
                    else {
                        pszIdColumn = CPLStrdup(PQgetvalue(poResult, 0, 0));
                    }
                }
                else {
                    pszIdColumn = CPLStrdup(PQgetvalue(poResult, 0, 0));
                }

                PQclear(poResult);

                if (pszIdColumn == NULL) {
                    if (pszWhere == NULL) {
                        osCommand.Printf("select ST_UpperLeftX(%s), ST_UpperLeftY(%s) from %s.%s", pszColumn, 
                            pszColumn, pszSchema, pszTable);
                    }
                    else {
                        osCommand.Printf("select ST_UpperLeftX(%s), ST_UpperLeftY(%s) from %s.%s where %s", 
                            pszColumn, pszColumn, pszSchema, pszTable, pszWhere);
                    }
                    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
                        "Query: %s", osCommand.c_str());
                    poResult = PQexec(poConn, osCommand.c_str());
                    if (
                        poResult == NULL ||
                        PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                        PQntuples(poResult) <= 0
                        ) {
                        CPLError(CE_Failure, CPLE_AppDefined,
                            "Error retrieving raster row metadata");

                        if (poResult != NULL)
                            PQclear(poResult);

                        // pszIdColumn is alread null

                        return false;
                    }

                    nTuples = PQntuples(poResult);

                    for (i = 0; i < nTuples; i++) {
                        adfGeoTransform[0] = atof(PQgetvalue(poResult, i, 0)); //upperleft x
                        adfGeoTransform[3] = atof(PQgetvalue(poResult, i, 1)); //upperleft y
                        papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                                CPLSPrintf("SUBDATASET_%d_NAME", (i + 1)),
                                CPLSPrintf("PG:%s schema=%s table=%s column=%s "
                                    "where='ST_UpperLeftX(%s) = %f AND ST_UpperLeftY(%s) = %f'",
                                    pszValidConnectionString, pszSchema, pszTable, pszColumn, 
                                    pszColumn, adfGeoTransform[0], pszColumn, adfGeoTransform[3]));

                        papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                                CPLSPrintf("SUBDATASET_%d_DESC", (i + 1)),
                                CPLSPrintf("PostGIS Raster at %s.%s (%s), UpperLeft = %f, %f", pszSchema,
                                    pszTable, pszColumn, adfGeoTransform[0], adfGeoTransform[3]));
                    }

                    PQclear(poResult);
                }
                else {
                    if (pszWhere == NULL) {
                        osCommand.Printf("select %s from %s.%s", 
                            pszIdColumn, pszSchema, pszTable);
                    }
                    else {
                        osCommand.Printf("select %s from %s.%s where %s", 
                            pszIdColumn, pszSchema, pszTable, pszWhere);
                    }
                    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
                        "Query: %s", osCommand.c_str());
                    poResult = PQexec(poConn, osCommand.c_str());
                    if (
                        poResult == NULL ||
                        PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                        PQntuples(poResult) <= 0
                        ) {
                        CPLError(CE_Failure, CPLE_AppDefined,
                            "Error retrieving raster row metadata");

                        if (poResult != NULL)
                            PQclear(poResult);

                        if (pszIdColumn != NULL)
                            CPLFree(pszIdColumn);

                        return false;
                    }

                    nTuples = PQntuples(poResult);

                    for (i = 0; i < nTuples; i++) {
                        // this is the raster ID (or unique column)
                        nRasterID = atoi(PQgetvalue(poResult, i, 0));

                        papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                                CPLSPrintf("SUBDATASET_%d_NAME", (i + 1)),
                                CPLSPrintf("PG:%s schema=%s table=%s column=%s where='%s = %d'",
                                pszValidConnectionString, pszSchema, pszTable, pszColumn, 
                                pszIdColumn, nRasterID));

                        papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                                CPLSPrintf("SUBDATASET_%d_DESC", (i + 1)),
                                CPLSPrintf("PostGIS Raster at %s.%s (%s), %s = %d", pszSchema,
                                pszTable, pszColumn, pszIdColumn, nRasterID));
                    }

                    PQclear(poResult);
                }


                /* Not a single raster fetched */
                nRasterXSize = 0;
                nRasterYSize = 0;

                bRetValue = true;
    
                if (pszIdColumn != NULL)
                    CPLFree(pszIdColumn);

                }
                break;

                /************************************************************
                 * All rows form a whole raster coverage
                ************************************************************/
            case ONE_RASTER_PER_TABLE:
                {
                /**
                 * Get whole raster extent
                 **/
                if (pszWhere == NULL)
                    osCommand.Printf("select st_astext(st_setsrid(st_extent(%s::geometry),%d)) from %s.%s",
                        pszColumn, nSrid, pszSchema, pszTable);
                else
                    osCommand.Printf("select st_astext(st_setsrid(st_extent(%s::geometry),%d)) from %s.%s where %s",
                        pszColumn, nSrid, pszSchema, pszTable, pszWhere);


                poResult = PQexec(poConn, osCommand.c_str());
                if (poResult == NULL ||
                        PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                        PQntuples(poResult) <= 0) {
                    CPLError(CE_Failure, CPLE_AppDefined,
                            "Error calculating whole raster extent: %s",
                            PQerrorMessage(poConn));

                    if (poResult != NULL)
                        PQclear(poResult);

                    return false;
                }

                /* Construct an OGR object with the raster extent */
                pszExtent = PQgetvalue(poResult, 0, 0);

                pszProjectionRef = (char*) GetProjectionRef();
                poSR = new OGRSpatialReference(pszProjectionRef);
                OgrErr = OGRGeometryFactory::createFromWkt(&pszExtent,
                        poSR, &poGeom);
                if (OgrErr != OGRERR_NONE) {
                    CPLError(CE_Failure, CPLE_AppDefined,
                            "Couldn't calculate raster extent");

                    if (poResult != NULL)
                        PQclear(poResult);

                    return false;
                }

                poE = new OGREnvelope();
                poGeom->getEnvelope(poE);

                /* Correction for upper left y coord*/

                /**
                 * TODO: Review this. Is a good algorithm?
                 * If the pixel size Y is negative, we can assume the raster's
                 * reference system uses cartesian coordinates, in which the
                 * origin is in lower-left corner, while the origin in an image
                 * is un upper-left corner. In this case, the upper left Y value
                 * will be MaxY from the envelope. Otherwise, it will be MinY.
                 **/
                /*
                adfGeoTransform[0] = poE->MinX;
                if (adfGeoTransform[5] >= 0.0)
                    adfGeoTransform[3] = poE->MinY;
                else
                    adfGeoTransform[3] = poE->MaxY;
                */

                /**
                 * The raster size is the extent covered for all the raster's
                 * columns
                 **/
                nRasterXSize = (int)
                        fabs(rint((poE->MaxX - poE->MinX) / adfGeoTransform[1]));
                nRasterYSize = (int)
                        fabs(rint((poE->MaxY - poE->MinY) / adfGeoTransform[5]));


                /* Free resources */
                OGRGeometryFactory::destroyGeometry(poGeom);
                delete poE;
                delete poSR;
                PQclear(poResult);

                bRetValue = true;

                }
                break;

                /* TODO: take into account more cases, if applies */
            default:
                {
                CPLError(CE_Failure, CPLE_AppDefined,
                        "Error, incorrect working mode");

                bRetValue = false;
                }
        }
    }

    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
            "adfGeoTransform = {%f, %f, %f, %f, %f,%f}", adfGeoTransform[0],
            adfGeoTransform[1], adfGeoTransform[2], adfGeoTransform[3],
            adfGeoTransform[4], adfGeoTransform[5]);

    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
            "Raster size = (%d, %d)", nRasterXSize, nRasterYSize);


    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::SetRasterProperties(): "
            "Block dimensions = (%d x %d)", nBlockXSize, nBlockYSize);

    /* Do not set bands if properties error'd */
    if (!bRetValue) {
        return bRetValue;
    }

    /* Create query to fetch metadata from db */
    if (pszWhere == NULL) {
        osCommand.Printf("select st_bandpixeltype(rast, band), "
            "st_bandnodatavalue(rast, band) is null, "
            "st_bandnodatavalue(rast, band) from (select %s, "
            "generate_series(1, st_numbands(%s)) band from (select "
            "rast from %s.%s limit 1) bar) foo",
            pszColumn, pszColumn, pszSchema, pszTable);
    } else {
        osCommand.Printf("select st_bandpixeltype(rast, band), "
            "st_bandnodatavalue(rast, band) is null, "
            "st_bandnodatavalue(rast, band) from (select %s, "
            "generate_series(1, st_numbands(%s)) band from (select "
            "rast from %s.%s where %s limit 1) bar) foo",
            pszColumn, pszColumn, pszSchema, pszTable, pszWhere);
    }

    poResult = PQexec(poConn, osCommand.c_str());
    nTuples = PQntuples(poResult);

    /* Error getting info from database */
    if (poResult == NULL || PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            nTuples <= 0) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Error getting band metadata: %s",
                PQerrorMessage(poConn));
        if (poResult)
            PQclear(poResult);

        return false;
    }

    /* Create each PostGISRasterRasterBand using the band metadata */
    for (iBand = 0; iBand < nTuples; iBand++) {
        /**
         * If we have more than one record here is because there are several
         * rows, belonging to the same raster coverage, with different band
         * metadata values. An error must be raised.
         *
         * TODO: Is there any way to fix this problem?
         *
         * TODO: Even when the difference between metadata values are only a
         * few decimal numbers (for example: 3.0000000 and 3.0000001) they're
         * different tuples. And in that case, they must be the same
         **/
        /*
        if (nTuples > 1) {
            CPLError(CE_Failure, CPLE_AppDefined, "Error, the \
                    ONE_RASTER_PER_TABLE mode can't be applied if the raster \
                    rows don't have the same metadata for band %d",
                    iBand + 1);
            PQclear(poResult);
            return false;
        }
         */


        /* Get metadata and create raster band objects */
        pszDataType = CPLStrdup(PQgetvalue(poResult, iBand, 0));
        bHasNoDataValue = EQUALN(PQgetvalue(poResult, iBand, 1), "f", sizeof(char));
        dfNodata = atof(PQgetvalue(poResult, iBand, 2));
        /** 
         * Offline rasters are not yet supported. When offline rasters are
         * supported, they will also requires a fast 'getter', other than 
         * the ST_BandMetaData accessor.
         **/
        /*
        bIsOffline = EQUALN(PQgetvalue(poResult, iBand, 3), "t", sizeof (char));        */

        if (EQUALN(pszDataType, "1BB", 3 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 1;
        } else if (EQUALN(pszDataType, "2BUI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 2;
        } else if (EQUALN(pszDataType, "4BUI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 4;
        } else if (EQUALN(pszDataType, "8BUI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 8;
        } else if (EQUALN(pszDataType, "8BSI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            /**
             * To indicate the unsigned byte values between 128 and 255
             * should be interpreted as being values between -128 and -1 for
             * applications that recognise the SIGNEDBYTE type.
             **/
            bSignedByte = true;
            nBitDepth = 8;
        } else if (EQUALN(pszDataType, "16BSI", 5 * sizeof (char))) {
            hDataType = GDT_Int16;
            nBitDepth = 16;
        } else if (EQUALN(pszDataType, "16BUI", 5 * sizeof (char))) {
            hDataType = GDT_UInt16;
            nBitDepth = 16;
        } else if (EQUALN(pszDataType, "32BSI", 5 * sizeof (char))) {
            hDataType = GDT_Int32;
            nBitDepth = 32;
        } else if (EQUALN(pszDataType, "32BUI", 5 * sizeof (char))) {
            hDataType = GDT_UInt32;
            nBitDepth = 32;
        } else if (EQUALN(pszDataType, "32BF", 4 * sizeof (char))) {
            hDataType = GDT_Float32;
            nBitDepth = 32;
        } else if (EQUALN(pszDataType, "64BF", 4 * sizeof (char))) {
            hDataType = GDT_Float64;
            nBitDepth = 64;
        } else {
            hDataType = GDT_Byte;
            nBitDepth = 8;
        }

        /* Create raster band object */
        SetBand(iBand + 1, new PostGISRasterRasterBand(this, iBand + 1, hDataType,
                bHasNoDataValue, dfNodata, bSignedByte, nBitDepth, 0, bIsOffline));

        CPLFree(pszDataType);
    }

    PQclear(poResult);

    return true;
}

/**
 * Read/write a region of image data from multiple bands.
 *
 * This method allows reading a region of one or more PostGISRasterBands from
 * this dataset into a buffer. The write support is still under development
 *
 * The function fetches all the raster data that intersects with the region
 * provided, and store the data in the GDAL cache.
 *
 * TODO: This only works in case of regular blocking rasters. A more
 * general approach to allow non-regular blocking rasters is under development.
 *
 * It automatically takes care of data type translation if the data type
 * (eBufType) of the buffer is different than that of the
 * PostGISRasterRasterBand.
 *
 * TODO: The method should take care of image decimation / replication if the
 * buffer size (nBufXSize x nBufYSize) is different than the size of the region
 * being accessed (nXSize x nYSize).
 *
 * The nPixelSpace, nLineSpace and nBandSpace parameters allow reading into or
 * writing from various organization of buffers.
 *
 * @param eRWFlag Either GF_Read to read a region of data, or GF_Write to write
 * a region of data.
 *
 * @param nXOff The pixel offset to the top left corner of the region of the
 * band to be accessed. This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region of the band
 * to be accessed. This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param pData The buffer into which the data should be read, or from which it
 * should be written. This buffer must contain at least
 * nBufXSize * nBufYSize * nBandCount words of type eBufType. It is organized in
 * left to right,top to bottom pixel order. Spacing is controlled by the
 * nPixelSpace, and nLineSpace parameters.
 *
 * @param nBufXSize the width of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param nBufYSize the height of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param eBufType the type of the pixel values in the pData data buffer. The
 * pixel values will automatically be translated to/from the
 * PostGISRasterRasterBand data type as needed.
 *
 * @param nBandCount the number of bands being read or written.
 *
 * @param panBandMap the list of nBandCount band numbers being read/written.
 * Note band numbers are 1 based. This may be NULL to select the first
 * nBandCount bands.
 *
 * @param nPixelSpace The byte offset from the start of one pixel value in pData
 * to the start of the next pixel value within a scanline. If defaulted (0) the
 * size of the datatype eBufType is used.
 *
 * @param nLineSpace The byte offset from the start of one scanline in pData to
 * the start of the next. If defaulted (0) the size of the datatype
 * eBufType * nBufXSize is used.
 *
 * @param nBandSpace the byte offset from the start of one bands data to the
 * start of the next. If defaulted (0) the value will be nLineSpace * nBufYSize
 * implying band sequential organization of the data buffer.
 *
 * @return CE_Failure if the access fails, otherwise CE_None.
 */
CPLErr PostGISRasterDataset::IRasterIO(GDALRWFlag eRWFlag,
        int nXOff, int nYOff, int nXSize, int nYSize,
        void * pData, int nBufXSize, int nBufYSize,
        GDALDataType eBufType,
        int nBandCount, int *panBandMap,
        int nPixelSpace, int nLineSpace, int nBandSpace)
{
    double adfTransform[6];
    double adfProjWin[8];
    int ulx, uly, lrx, lry;
    CPLString osCommand;
    PGresult* poResult = NULL;
    int nTuples = 0;
    int iTuplesIndex = 0;
    GByte* pbyData = NULL;
    int nWKBLength = 0;
    int iBandIndex;
    GDALRasterBlock * poBlock = NULL;
    int iBlockXOff, iBlockYOff;
    int nBandDataSize, nBandDataLength;
    char * pBandData = NULL;
    PostGISRasterRasterBand * poBand = NULL;
    GByte * pabySrcBlock = NULL;
    int nBlocksPerRow, nBlocksPerColumn;
    char orderByY[5];
    char orderByX[4];


    /**
     * TODO: Write support not implemented yet
     **/
    if (eRWFlag == GF_Write)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                "PostGIS Raster does not support writing");
        return CE_Failure;
    }

    /**
     * TODO: Data decimation / replication needed
     */
    if (nBufXSize != nXSize || nBufYSize != nYSize)
    {
        /**
         * This will cause individual IReadBlock calls
         *
         */
        return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                pData, nBufXSize, nBufYSize, eBufType, nBandCount,
                panBandMap, nPixelSpace, nLineSpace, nBandSpace);
    }
        
    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO: "
            "nBandSpace = %d, nLineSpace = %d, nPixelSpace = %d",
            nBandSpace, nLineSpace, nPixelSpace);

    /**************************************************************************
     * In the first call, we fetch the data from database and store it as
     * 'blocks' in the cache.
     *
     * TODO: If the data is not cached, we must 'invent' a good block size, and
     * divide the data in blocks. To get a proper block size, we should rewrite
     * the GetBlockSize function at band level.
     ***************************************************************************/
    if (!bBlocksCached)
    {
        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO: "
                "Buffer size = (%d, %d), Region size = (%d, %d)",
                nBufXSize, nBufYSize, nXSize, nYSize);

        /*******************************************************************
         * Construct a projected window to intersect the band data
         *******************************************************************/
        GetGeoTransform(adfTransform);
        ulx = nXOff;
        uly = nYOff;
        lrx = nXOff + nXSize;
        lry = nYOff + nYSize;

        /* Calculate the intersection polygon */
        adfProjWin[0] = adfTransform[0] +
            ulx * adfTransform[1] +
            uly * adfTransform[2];

        adfProjWin[1] = adfTransform[3] +
            ulx * adfTransform[4] +
            uly * adfTransform[5];

        adfProjWin[2] = adfTransform[0] +
            lrx * adfTransform[1] +
            uly * adfTransform[2];

        adfProjWin[3] = adfTransform[3] +
            lrx * adfTransform[4] +
            uly * adfTransform[5];

        adfProjWin[4] = adfTransform[0] +
            lrx * adfTransform[1] +
            lry * adfTransform[2];

        adfProjWin[5] = adfTransform[3] +
            lrx * adfTransform[4] +
            lry * adfTransform[5];

        adfProjWin[6] = adfTransform[0] +
            ulx * adfTransform[1] +
            lry * adfTransform[2];

        adfProjWin[7] = adfTransform[3] +
            ulx * adfTransform[4] +
            lry * adfTransform[5];


        /* Construct order by for the query */
        memset(orderByX, 0, 4);
        memset(orderByY, 0, 5);

        strcpy(orderByX, "asc");
        if (nSrid == -1)
            strcpy(orderByY, "asc"); // Y starts at 0 and grows
        else
            strcpy(orderByY, "desc");// Y starts at max and decreases
 

        /*********************************************************************
        * We first get the data from database (ordered from upper left pixel
        * to lower right one)
        *********************************************************************/
        if (pszWhere == NULL)
        {
            osCommand.Printf("SELECT %s, ST_ScaleX(%s), ST_SkewY(%s), "
                "ST_SkewX(%s), ST_ScaleY(%s), ST_UpperLeftX(%s), "
                "ST_UpperLeftY(%s), ST_Width(%s), ST_Height(%s) FROM %s.%s WHERE "
                "ST_Intersects(%s, ST_PolygonFromText('POLYGON((%.17f %.17f, "
                "%.17f %.17f, %.17f %.17f, %.17f %.17f, %.17f %.17f))', %d)) "
                "ORDER BY ST_UpperLeftY(%s) %s, ST_UpperLeftX(%s) %s", 
                pszColumn, pszColumn, pszColumn, pszColumn, pszColumn, 
                pszColumn, pszColumn, pszColumn, pszColumn, pszSchema, 
                pszTable, pszColumn, adfProjWin[0], adfProjWin[1], 
                adfProjWin[2], adfProjWin[3], adfProjWin[4], adfProjWin[5], 
                adfProjWin[6], adfProjWin[7], adfProjWin[0], adfProjWin[1], 
                nSrid, pszColumn, orderByY, pszColumn, orderByX);
        }


        else
        {
            osCommand.Printf("SELECT %s, ST_ScaleX(%s), ST_SkewY(%s), "
                "ST_SkewX(%s), ST_ScaleY(%s), ST_UpperLeftX(%s), "
                "ST_UpperLeftY(%s), ST_Width(%s), ST_Height(%s) FROM %s.%s WHERE %s AND "
                "ST_Intersects(%s, ST_PolygonFromText('POLYGON((%.17f %.17f, "
                "%.17f %.17f, %.17f %.17f, %.17f %.17f, %.17f %.17f))', %d)) "
                "ORDER BY ST_UpperLeftY(%s) %s, ST_UpperLeftX(%s) %s", 
                pszColumn, pszColumn, pszColumn, pszColumn, pszColumn, 
                pszColumn, pszColumn, pszColumn, pszColumn, pszSchema, 
                pszTable, pszWhere, pszColumn, adfProjWin[0], adfProjWin[1], 
                adfProjWin[2], adfProjWin[3], adfProjWin[4], adfProjWin[5], 
                adfProjWin[6], adfProjWin[7], adfProjWin[0], adfProjWin[1], 
                nSrid, pszColumn, orderByY, pszColumn, orderByX);
        }

        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): Query = %s",
            osCommand.c_str());

        poResult = PQexec(poConn, osCommand.c_str());
        if (poResult == NULL || PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            PQntuples(poResult) <= 0)
        {
            if (poResult)
                PQclear(poResult);

            /**
             * This will cause individual IReadBlock calls
             *
             */
            return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                    pData, nBufXSize, nBufYSize, eBufType, nBandCount,
                    panBandMap, nPixelSpace, nLineSpace, nBandSpace);
        }

        /**
         * NOTE: In case of any of the raster columns have different SRID, the
         * query will fail. So, if we don't fail, we can assume all the rows
         * have the same SRID. We don't need to check it
         **/


        nTuples = PQntuples(poResult);
        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): nTuples = %d",
            nTuples);

        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): "
            "Raster size = (%d, %d)", nRasterXSize, nRasterYSize);



        /**************************************************************************
         * This is the simplest case: all the rows have the same dimensions
         * (regularly blocked raster)
         *
         * Now we'll cache each tuple as a data block. More accurately, we must
         * access each tuple, get the band data, and store this data as a block. So,
         * each tuple contains the data for nBands blocks (and nBandCount <=
         * nBands)
         *************************************************************************/
        for(iBandIndex = 0; iBandIndex < nBandCount; iBandIndex++)
        {
            poBand = (PostGISRasterRasterBand *)GetRasterBand(iBandIndex + 1);

            nBandDataSize = GDALGetDataTypeSize(poBand->eDataType) / 8;

            nBandDataLength = poBand->nBlockXSize * poBand->nBlockYSize *
                nBandDataSize;

            CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): "
                "Block size (%d, %d) for band %d", poBand->nBlockXSize,
                poBand->nBlockYSize, poBand->nBand);

            /* Enables block caching, if it wasn't enabled */
            if (!poBand->InitBlockInfo())
                continue;

            /**
             * This can be different from poBand->nBlocksPerRow and
             * poBand->nBlocksPerColumn, if the region size is different than
             * the raster size. So, we calculate these values for this case.
             */
            nBlocksPerRow =
                    (nXSize + poBand->nBlockXSize - 1) / poBand->nBlockXSize;

            nBlocksPerColumn =
                    (nYSize + poBand->nBlockYSize - 1) / poBand->nBlockYSize;

            CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): "
                "Number of blocks: %dx%d", nBlocksPerRow, nBlocksPerColumn);

            for(iBlockYOff = 0; iBlockYOff < nBlocksPerColumn;
                    iBlockYOff++)
            {
                for(iBlockXOff = 0; iBlockXOff < nBlocksPerRow;
                        iBlockXOff++)
                {
                    iTuplesIndex = (iBlockYOff * nBlocksPerRow) +
                        iBlockXOff;

                    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): "
                            "iBlockXOff = %d, iBlockYOff = %d, "
                            "iTuplesIndex = %d", iBlockXOff, iBlockYOff,
                            iTuplesIndex);

                    pbyData = CPLHexToBinary(PQgetvalue(poResult, iTuplesIndex,
                            1), &nWKBLength);

                    pBandData = (char *)GET_BAND_DATA(pbyData, poBand->nBand,
                            nBandDataSize, nBandDataLength);

                    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): "
                        "Block data length for band %d: %d", poBand->nBand,
                        nBandDataLength);

                    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::IRasterIO(): "
                        "Block (%d, %d)", iBlockXOff, iBlockYOff);

                    /* Create a new block */
                    poBlock = new GDALRasterBlock(poBand, iBlockXOff,
                            iBlockYOff);

                    poBlock->AddLock();

                    /* Allocate data space */
                    if (poBlock->Internalize() != CE_None)
                    {
                        poBlock->DropLock();
                        delete poBlock;
                        continue;
                    }

                    /* Add the block to the block matrix */
                    if (poBand->AdoptBlock(iBlockXOff, iBlockYOff, poBlock) !=
                            CE_None)
                    {
                        poBlock->DropLock();
                        delete poBlock;
                        continue;
                    }

                    /**
                     * Copy data to block
                     *
                     * TODO: Enable write mode too (mark the block as dirty and
                     * create IWriteBlock in PostGISRasterRasterBand)
                     */
                    pabySrcBlock = (GByte *)poBlock->GetDataRef();

                    if (poBand->eDataType == eBufType)
                    {
                        memcpy(pabySrcBlock, pBandData, nBandDataLength);
                    }

                    /**
                     * As in GDALDataset class... expensive way of handling
                     * single words
                     */
                    else
                    {
                        GDALCopyWords(pBandData, poBand->eDataType, 0,
                                pabySrcBlock, eBufType, 0, 1);
                    }

                    poBlock->DropLock();

                    CPLFree(pbyData);
                    pbyData = NULL;
                }

            }

        }

        PQclear(poResult);
        bBlocksCached = true;
    }

    /* Once the blocks are cached, we delegate in GDAL I/O system */
    return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
            nBufXSize, nBufYSize, eBufType, nBandCount, panBandMap, nPixelSpace,
            nLineSpace, nBandSpace);
}

/******************************************************************************
 * \brief Get the connection information for a filename.
 ******************************************************************************/
static GBool
GetConnectionInfo(const char * pszFilename, 
    char ** ppszConnectionString, char ** ppszSchema, char ** ppszTable, 
    char ** ppszColumn, char ** ppszWhere, char ** ppszHost, 
    char ** ppszPort, char ** ppszUser, char ** ppszPassword, 
    int * nMode, GBool * bBrowseDatabase)
{
    int nPos = -1, i;
    char * pszTmp = NULL;
    char **papszParams = ParseConnectionString(pszFilename);
    if (papszParams == NULL) {
        return false;
    }

    /**************************************************************************
     * Get mode:
     *  - 1. ONE_RASTER_PER_ROW: Each row is considered as a separate raster
     *  - 2. ONE_RASTER_PER_TABLE: All the table rows are considered as a whole
     *      raster coverage
     **************************************************************************/
    nPos = CSLFindName(papszParams, "mode");
    if (nPos != -1) {
        *nMode = atoi(CPLParseNameValue(papszParams[nPos], NULL));

        if (*nMode != ONE_RASTER_PER_ROW && *nMode != ONE_RASTER_PER_TABLE) {
            /* Unrecognized mode, using default one */
            /*
            CPLError(CE_Warning, CPLE_AppDefined, "Undefined working mode (%d)."
                    " Valid working modes are 1 (ONE_RASTER_PER_ROW) and 2"
                    " (ONE_RASTER_PER_TABLE). Using ONE_RASTER_PER_TABLE"
                    " by default", nMode);
             */
            *nMode = ONE_RASTER_PER_ROW;
        }

        /* Remove the mode from connection string */
        papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
    }
        /* Default mode */
    else
        *nMode = ONE_RASTER_PER_ROW;

    /**
     * Case 1: There's no database name: Error, you need, at least,
     * specify a database name (NOTE: insensitive search)
     **/
    nPos = CSLFindName(papszParams, "dbname");
    if (nPos == -1) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "You must specify at least a db name");

        CSLDestroy(papszParams);

        return false;
    }

    /**
     * Case 2: There's database name, but no table name: activate a flag
     * for browsing the database, fetching all the schemas that contain
     * raster tables
     **/
    nPos = CSLFindName(papszParams, "table");
    if (nPos == -1) {
        *bBrowseDatabase = true;

        /* Get schema name, if exist */
        nPos = CSLFindName(papszParams, "schema");
        if (nPos != -1) {
            *ppszSchema = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        /**
         * Remove the rest of the parameters, if exist (they mustn't be present
         * if we want a valid PQ connection string)
         **/
        nPos = CSLFindName(papszParams, "column");
        if (nPos != -1) {
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        nPos = CSLFindName(papszParams, "where");
        if (nPos != -1) {
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }
    } else {
        *ppszTable = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
        /* Delete this pair from params array */
        papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);

        /**
         * Case 3: There's database and table name, but no column
         * name: Use a default column name and use the table to create the
         * dataset
         **/
        nPos = CSLFindName(papszParams, "column");
        if (nPos == -1) {
            *ppszColumn = CPLStrdup(DEFAULT_COLUMN);
        }
        /**
         * Case 4: There's database, table and column name: Use the table to
         * create a dataset
         **/
        else {
            *ppszColumn = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        /* Get the rest of the parameters, if exist */
        nPos = CSLFindName(papszParams, "schema");
        if (nPos == -1) {
            *ppszSchema = CPLStrdup(DEFAULT_SCHEMA);
        } else {
            *ppszSchema = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        nPos = CSLFindName(papszParams, "where");
        if (nPos != -1) {
            *ppszWhere = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }
    }

    /* Parse ppszWhere, if needed */
    if (*ppszWhere) {
        pszTmp = ReplaceQuotes(*ppszWhere, strlen(*ppszWhere));
        CPLFree(*ppszWhere);
        *ppszWhere = pszTmp;
    }

    /***************************************
     * Construct a valid connection string
     ***************************************/
    *ppszConnectionString = (char*) CPLCalloc(strlen(pszFilename),
            sizeof (char));
    for (i = 0; i < CSLCount(papszParams); i++) {
        *ppszConnectionString = strncat(*ppszConnectionString, papszParams[i], strlen(papszParams[i]));
        *ppszConnectionString = strncat(*ppszConnectionString, " ", strlen(" "));
    }

    nPos = CSLFindName(papszParams, "host");
    if (nPos != -1) {
        *ppszHost = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
    }
    else if (getenv("PGHOST") != NULL) {
        *ppszHost = CPLStrdup(getenv("PGHOST"));
    }
    else {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Host parameter must be provided, or PGHOST environment "
                "variable must be set. Please set the host and try again.");

        CSLDestroy(papszParams);

        return false;
    }

    nPos = CSLFindName(papszParams, "port");
    if (nPos != -1) {
        *ppszPort = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
    }
    else if (getenv("PGPORT") != NULL ) {
        *ppszPort = CPLStrdup(getenv("PGPORT"));
    }
    else {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Port parameter must be provided, or PGPORT environment "
                "variable must be set. Please set the port and try again.");

        CSLDestroy(papszParams);

        return false;
    }

    nPos = CSLFindName(papszParams, "user");
    if (nPos != -1) {
        *ppszUser = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
    }
    else if (getenv("PGUSER") != NULL ) {
        *ppszUser = CPLStrdup(getenv("PGUSER"));
    }
    else {
        CPLError(CE_Failure, CPLE_AppDefined,
                "User parameter must be provided, or PGUSER environment "
                "variable must be set. Please set the user and try again.");

        CSLDestroy(papszParams);

        return false;
    }

    nPos = CSLFindName(papszParams, "password");
    if (nPos != -1) {
        *ppszPassword = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
    }
    else {
        // if PGPASSWORD is not set, ppszPassword is set to an empty string.
        // this is okay, since there may be configurations in pg_hba.conf
        // that don't require any password to connect
        *ppszPassword = CPLStrdup(getenv("PGPASSWORD"));
    }

    CSLDestroy(papszParams);

    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::GetConnectionInfo(): "
        "Mode: %d\nSchema: %s\nTable: %s\nColumn: %s\nWhere: %s\n"
        "Host: %s\nPort: %s\nUser: %s\nPassword: %s\nConnection String: %s", 
        *nMode, *ppszSchema, *ppszTable, *ppszColumn, 
        *ppszWhere, *ppszHost, *ppszPort, *ppszUser, *ppszPassword, *ppszConnectionString);

    return true;
}

/******************************************************************************
 * \brief Create a connection to a postgres database
 ******************************************************************************/
static PGconn *
GetConnection(const char * pszFilename, char ** ppszConnectionString,
    char ** ppszSchema, char ** ppszTable, char ** ppszColumn, char ** ppszWhere, 
    int * nMode, GBool * bBrowseDatabase) 
{
    PostGISRasterDriver * poDriver;
    PGconn * poConn = NULL;
    char * pszHost = NULL;
    char * pszPort = NULL;
    char * pszUser = NULL;
    char * pszPassword = NULL;

    if (GetConnectionInfo(pszFilename, ppszConnectionString, ppszSchema, 
        ppszTable, ppszColumn, ppszWhere, &pszHost, &pszPort, &pszUser, 
        &pszPassword, nMode, bBrowseDatabase)) 
    {
        /********************************************************************
         * Open a new database connection
         ********************************************************************/
        poDriver = (PostGISRasterDriver *)GDALGetDriverByName("PostGISRaster");
        poConn = poDriver->GetConnection(*ppszConnectionString,
                pszHost, pszPort, pszUser, pszPassword);

        if (poConn == NULL) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Couldn't establish a database connection");
        }
    }

    CPLFree(pszHost);
    CPLFree(pszPort);
    CPLFree(pszUser);
    CPLFree(pszPassword);

    return poConn;
}


/******************************************************************************
 * \brief Open a connection with PostgreSQL. The connection string will have
 * the PostgreSQL accepted format, plus the next key=value pairs:
 *	schema = <schema_name>
 *	table = <table_name>
 *	column = <column_name>
 *	where = <SQL where>
 *  mode = <working mode> (1 or 2)
 *
 * These pairs are used for selecting the right raster table.
 *****************************************************************************/
GDALDataset* PostGISRasterDataset::Open(GDALOpenInfo* poOpenInfo) {
    char* pszConnectionString = NULL;
    char* pszSchema = NULL;
    char* pszTable = NULL;
    char* pszColumn = NULL;
    char* pszWhere = NULL;
    int nMode = -1;
    PGconn * poConn = NULL;
    PostGISRasterDataset* poDS = NULL;
    GBool bBrowseDatabase = false;
    CPLString osCommand;

    /**************************
     * Check input parameter
     **************************/
    if (poOpenInfo->pszFilename == NULL ||
            poOpenInfo->fp != NULL ||
            !EQUALN(poOpenInfo->pszFilename, "PG:", 3))
    {
        /**
         * Drivers must quietly return NULL if the passed file is not of
         * their format. They should only produce an error if the file
         * does appear to be of their supported format, but for some
         * reason, unsupported or corrupt
         */
        return NULL;
    }

    poConn = GetConnection((char *)poOpenInfo->pszFilename,
        &pszConnectionString, &pszSchema, &pszTable, &pszColumn, &pszWhere,
        &nMode, &bBrowseDatabase);
    if (poConn == NULL) {
        CPLFree(pszConnectionString);
        CPLFree(pszSchema);
        CPLFree(pszTable);
        CPLFree(pszColumn);
        CPLFree(pszWhere);
        return NULL;
    }

    /*************************************************************************
     * No table will be read. Only shows information about the existent raster
     * tables
     *************************************************************************/
    if (bBrowseDatabase) {
        /**
         * Creates empty dataset object, only for subdatasets
         **/
        poDS = new PostGISRasterDataset();
        poDS->poConn = poConn;
        poDS->eAccess = GA_ReadOnly;
        //poDS->poDriver = poDriver;
        poDS->nMode = (pszSchema) ? BROWSE_SCHEMA : BROWSE_DATABASE;
        poDS->nRasterXSize = 0;
        poDS->nRasterYSize = 0;
        poDS->adfGeoTransform[0] = 0.0;
        poDS->adfGeoTransform[1] = 0.0;
        poDS->adfGeoTransform[2] = 0.0;
        poDS->adfGeoTransform[3] = 0.0;
        poDS->adfGeoTransform[4] = 0.0;
        poDS->adfGeoTransform[5] = 0.0;

        /**
         * Look for raster tables at database and
         * store them as subdatasets
         **/
        if (!poDS->BrowseDatabase(pszSchema, pszConnectionString)) {
            CPLFree(pszConnectionString);
            delete poDS;

            if (pszSchema)
                CPLFree(pszSchema);
            if (pszTable)
                CPLFree(pszTable);
            if (pszColumn)
                CPLFree(pszColumn);
            if (pszWhere)
                CPLFree(pszWhere);

            return NULL;
        }

        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);
    }
        /***********************************************************************
         * A table will be read: Fetch raster properties from db. Pay attention
         * to the block size: if the raster is blocked at database, the block
         * size can be fetched from each block size, if regular blocking table
         **********************************************************************/
    else {
        poDS = new PostGISRasterDataset();
        poDS->poConn = poConn;
        poDS->eAccess = poOpenInfo->eAccess;
        poDS->nMode = nMode;
        //poDS->poDriver = poDriver;

        poDS->pszSchema = pszSchema;
        poDS->pszTable = pszTable;
        poDS->pszColumn = pszColumn;
        poDS->pszWhere = pszWhere;

        /**
         * Fetch basic raster metadata from db
         **/

        if (!poDS->SetRasterProperties(pszConnectionString)) {
            CPLFree(pszConnectionString);
            delete poDS;
            return NULL;
        }

    }

    CPLFree(pszConnectionString);
    return poDS;

}

/*****************************************
 * \brief Get Metadata from raster
 * TODO: Add more options (the result of
 * calling ST_Metadata, for example)
 *****************************************/
char** PostGISRasterDataset::GetMetadata(const char *pszDomain) {
    if (pszDomain != NULL && EQUALN(pszDomain, "SUBDATASETS", 11))
        return papszSubdatasets;
    else
        return GDALDataset::GetMetadata(pszDomain);
}

/*****************************************************
 * \brief Fetch the projection definition string
 * for this dataset in OpenGIS WKT format. It should
 * be suitable for use with the OGRSpatialReference
 * class.
 *****************************************************/
const char* PostGISRasterDataset::GetProjectionRef() {
    CPLString osCommand;
    PGresult* poResult;

    if (nSrid == -1)
        return "";

    if (pszProjection)
        return pszProjection;

    /********************************************************
     *          Reading proj from database
     ********************************************************/
    osCommand.Printf("SELECT srtext FROM spatial_ref_sys where SRID=%d",
            nSrid);
    poResult = PQexec(this->poConn, osCommand.c_str());
    if (poResult && PQresultStatus(poResult) == PGRES_TUPLES_OK
            && PQntuples(poResult) > 0) {
        /*
         * TODO: Memory leak detected with valgrind caused by allocation here.
         * Even when the return string is freed
         */
        pszProjection = CPLStrdup(PQgetvalue(poResult, 0, 0));
    }

    if (poResult)
        PQclear(poResult);

    return pszProjection;
}

/**********************************************************
 * \brief Set projection definition. The input string must
 * be in OGC WKT or PROJ.4 format
 **********************************************************/
CPLErr PostGISRasterDataset::SetProjection(const char * pszProjectionRef) {
    VALIDATE_POINTER1(pszProjectionRef, "SetProjection", CE_Failure);

    CPLString osCommand;
    PGresult * poResult;
    int nFetchedSrid = -1;


    /*****************************************************************
     * Check if the dataset allows updating
     *****************************************************************/
    if (GetAccess() != GA_Update) {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                "This driver doesn't allow write access");
        return CE_Failure;
    }

    /*****************************************************************
     * Look for projection with this text
     *****************************************************************/

    // First, WKT text
    osCommand.Printf("SELECT srid FROM spatial_ref_sys where srtext='%s'",
            pszProjectionRef);
    poResult = PQexec(poConn, osCommand.c_str());

    if (poResult && PQresultStatus(poResult) == PGRES_TUPLES_OK
            && PQntuples(poResult) > 0) {

        nFetchedSrid = atoi(PQgetvalue(poResult, 0, 0));

        // update class attribute
        nSrid = nFetchedSrid;


        // update raster_columns table
        osCommand.Printf("UPDATE raster_columns SET srid=%d WHERE \
                    r_table_name = '%s' AND r_column = '%s'",
                nSrid, pszTable, pszColumn);
        poResult = PQexec(poConn, osCommand.c_str());
        if (poResult == NULL || PQresultStatus(poResult) != PGRES_COMMAND_OK) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Couldn't update raster_columns table: %s",
                    PQerrorMessage(poConn));
            return CE_Failure;
        }

        // TODO: Update ALL blocks with the new srid...

        return CE_None;
    }
        // If not, proj4 text
    else {
        osCommand.Printf(
                "SELECT srid FROM spatial_ref_sys where proj4text='%s'",
                pszProjectionRef);
        poResult = PQexec(poConn, osCommand.c_str());

        if (poResult && PQresultStatus(poResult) == PGRES_TUPLES_OK
                && PQntuples(poResult) > 0) {

            nFetchedSrid = atoi(PQgetvalue(poResult, 0, 0));

            // update class attribute
            nSrid = nFetchedSrid;

            // update raster_columns table
            osCommand.Printf("UPDATE raster_columns SET srid=%d WHERE \
                    r_table_name = '%s' AND r_column = '%s'",
                    nSrid, pszTable, pszColumn);

            poResult = PQexec(poConn, osCommand.c_str());
            if (poResult == NULL ||
                    PQresultStatus(poResult) != PGRES_COMMAND_OK) {
                CPLError(CE_Failure, CPLE_AppDefined,
                        "Couldn't update raster_columns table: %s",
                        PQerrorMessage(poConn));
                return CE_Failure;
            }

            // TODO: Update ALL blocks with the new srid...

            return CE_None;
        }
        else {
            CPLError(CE_Failure, CPLE_WrongFormat,
                    "Couldn't find WKT neither proj4 definition");
            return CE_Failure;
        }
    }
}

/********************************************************
 * \brief Set the affine transformation coefficients
 ********************************************************/
CPLErr PostGISRasterDataset::SetGeoTransform(double* padfTransform) {
    if (!padfTransform)
        return CE_Failure;

    adfGeoTransform[0] = padfTransform[0];
    adfGeoTransform[1] = padfTransform[1];
    adfGeoTransform[2] = padfTransform[2];
    adfGeoTransform[3] = padfTransform[3];
    adfGeoTransform[4] = padfTransform[4];
    adfGeoTransform[5] = padfTransform[5];

    return CE_None;
}

/********************************************************
 * \brief Get the affine transformation coefficients
 ********************************************************/
CPLErr PostGISRasterDataset::GetGeoTransform(double * padfTransform) {
    // copy necessary values in supplied buffer
    padfTransform[0] = adfGeoTransform[0];
    padfTransform[1] = adfGeoTransform[1];
    padfTransform[2] = adfGeoTransform[2];
    padfTransform[3] = adfGeoTransform[3];
    padfTransform[4] = adfGeoTransform[4];
    padfTransform[5] = adfGeoTransform[5];

    return CE_None;
}

/********************************************************
 * \brief Create a copy of a PostGIS Raster dataset.
 ********************************************************/
GDALDataset * 
PostGISRasterDataset::CreateCopy( const char * pszFilename,
    GDALDataset *poGSrcDS, int bStrict, char ** papszOptions, 
    GDALProgressFunc pfnProgress, void * pProgressData ) 
{
    char* pszSchema = NULL;
    char* pszTable = NULL;
    char* pszColumn = NULL;
    char* pszWhere = NULL;
    GBool bBrowseDatabase = false;
    int nMode;
    char* pszConnectionString = NULL;
    const char* pszSubdatasetName;
    PGconn * poConn = NULL;
    PGresult * poResult = NULL;
    CPLString osCommand;
    GBool bInsertSuccess;
    PostGISRasterDataset *poSrcDS = (PostGISRasterDataset *)poGSrcDS;
    PostGISRasterDataset *poSubDS;

    // Check connection string
    if (pszFilename == NULL ||
        !EQUALN(pszFilename, "PG:", 3)) {
        /**
         * The connection string provided is not a valid connection 
         * string.
         */
        CPLError( CE_Failure, CPLE_NotSupported, 
            "PostGIS Raster driver was unable to parse the provided "
            "connection string." );
        return NULL;
    }

    poConn = GetConnection(pszFilename, &pszConnectionString, &pszSchema, 
        &pszTable, &pszColumn, &pszWhere, &nMode, &bBrowseDatabase);
    if (poConn == NULL || bBrowseDatabase || pszTable == NULL) 
    {
        CPLFree(pszConnectionString);
        CPLFree(pszSchema);
        CPLFree(pszTable);
        CPLFree(pszColumn);
        CPLFree(pszWhere);

        // if connection info fails, browsing mode, or no table set
        return NULL;
    }

    // begin transaction
    poResult = PQexec(poConn, "begin");
    if (poResult == NULL ||
        PQresultStatus(poResult) != PGRES_COMMAND_OK) {
        CPLError(CE_Failure, CPLE_AppDefined,
            "Error beginning database transaction: %s",
            PQerrorMessage(poConn));
        if (poResult != NULL)
            PQclear(poResult);
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);

        CPLFree(pszConnectionString);

        return NULL;
    }

    PQclear(poResult);

    // create table for raster (if not exists because a
    // dataset will not be reported for an empty table)

    // TODO: is 'rid' necessary?
    osCommand.Printf("create table if not exists %s.%s (rid serial, %s "
        "public.raster, constraint %s_pkey primary key (rid));",
        pszSchema, pszTable, pszColumn, pszTable);
    poResult = PQexec(poConn, osCommand.c_str());
    if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {

        CPLError(CE_Failure, CPLE_AppDefined,
                "Error creating needed tables: %s",
                PQerrorMessage(poConn));
        if (poResult != NULL)
            PQclear(poResult);

        // rollback
        poResult = PQexec(poConn, "rollback");
        if (poResult == NULL ||
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {

            CPLError(CE_Failure, CPLE_AppDefined,
                "Error rolling back transaction: %s",
                PQerrorMessage(poConn));
        }
        if (poResult != NULL)
            PQclear(poResult);
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);

        CPLFree(pszConnectionString);

        return NULL;
    }

    PQclear(poResult);

    osCommand.Printf("create index %s_%s_gist ON %s.%s USING gist "
        "(public.st_convexhull(%s));", pszTable, pszColumn, 
        pszSchema, pszTable, pszColumn);
    poResult = PQexec(poConn, osCommand.c_str());
    if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {

        CPLError(CE_Failure, CPLE_AppDefined,
                "Error creating needed index: %s",
                PQerrorMessage(poConn));
        if (poResult != NULL)
            PQclear(poResult);

        // rollback
        poResult = PQexec(poConn, "rollback");
        if (poResult == NULL ||
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {

            CPLError(CE_Failure, CPLE_AppDefined,
                "Error rolling back transaction: %s",
                PQerrorMessage(poConn));
        }
        if (poResult != NULL)
            PQclear(poResult);
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);

        CPLFree(pszConnectionString);

        return NULL;
    }

    PQclear(poResult);

    if (poSrcDS->nMode == ONE_RASTER_PER_TABLE) {
        // one raster per table

        // insert one raster
        bInsertSuccess = InsertRaster(poConn, poSrcDS,
            pszSchema, pszTable, pszColumn);
        if (!bInsertSuccess) {
            // rollback
            poResult = PQexec(poConn, "rollback");
            if (poResult == NULL ||
                PQresultStatus(poResult) != PGRES_COMMAND_OK) {

                CPLError(CE_Failure, CPLE_AppDefined,
                    "Error rolling back transaction: %s",
                    PQerrorMessage(poConn));
            }
            if (poResult != NULL)
                PQclear(poResult);
            if (pszSchema)
                CPLFree(pszSchema);
            if (pszTable)
                CPLFree(pszTable);
            if (pszColumn)
                CPLFree(pszColumn);
            if (pszWhere)
                CPLFree(pszWhere);

            CPLFree(pszConnectionString);

            return NULL;
        }
    }
    else if (poSrcDS->nMode == ONE_RASTER_PER_ROW) {
        // one raster per row

        // papszSubdatasets contains name/desc for each subdataset
        for (int i = 0; i < CSLCount(poSrcDS->papszSubdatasets); i += 2) {
            pszSubdatasetName = CPLParseNameValue( poSrcDS->papszSubdatasets[i], NULL);
            if (pszSubdatasetName == NULL) {
                CPLDebug("PostGIS_Raster", "PostGISRasterDataset::CreateCopy(): "
                "Could not parse name/value out of subdataset list: "
                "%s", poSrcDS->papszSubdatasets[i]);
                continue;
            }

            // for each subdataset
            GDALOpenInfo poOpenInfo( pszSubdatasetName, GA_ReadOnly );
            // open the subdataset
            poSubDS = (PostGISRasterDataset *)Open(&poOpenInfo);

            if (poSubDS == NULL) {
                // notify!
                CPLDebug("PostGIS_Raster", "PostGISRasterDataset::CreateCopy(): "
                    "Could not open a subdataset: %s", 
                    pszSubdatasetName);
                continue;
            }

            // insert one raster
            bInsertSuccess = InsertRaster(poConn, poSubDS,
                pszSchema, pszTable, pszColumn);

            if (!bInsertSuccess) {
                CPLDebug("PostGIS_Raster", "PostGISRasterDataset::CreateCopy(): "
                    "Could not copy raster subdataset to new dataset." );

                // keep trying ...
            }

            // close this dataset
            GDALClose((GDALDatasetH)poSubDS);
        }
    }

    // commit transaction
    poResult = PQexec(poConn, "commit");
    if (poResult == NULL ||
        PQresultStatus(poResult) != PGRES_COMMAND_OK) {
        CPLError(CE_Failure, CPLE_AppDefined,
            "Error committing database transaction: %s",
            PQerrorMessage(poConn));
        if (poResult != NULL)
            PQclear(poResult);
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);

        CPLFree(pszConnectionString);

        return NULL;
    }

    PQclear(poResult);

    if (pszSchema)
        CPLFree(pszSchema);
    if (pszTable)
        CPLFree(pszTable);
    if (pszColumn)
        CPLFree(pszColumn);
    if (pszWhere)
        CPLFree(pszWhere);

    CPLFree(pszConnectionString);

    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::CreateCopy(): "
        "Opening new dataset: %s", pszFilename);

    // connect to the new dataset
    GDALOpenInfo poOpenInfo( pszFilename, GA_Update );
    // open the newdataset
    poSubDS = (PostGISRasterDataset *)Open(&poOpenInfo);

    if (poSubDS == NULL) {
        CPLDebug("PostGIS_Raster", "PostGISRasterDataset::CreateCopy(): "
            "New dataset could not be opened.");
    }

    return poSubDS;
}

/********************************************************
 * \brief Helper method to insert a new raster.
 ********************************************************/
GBool 
PostGISRasterDataset::InsertRaster(PGconn * poConn, 
    PostGISRasterDataset * poSrcDS, const char *pszSchema, 
    const char * pszTable, const char * pszColumn)
{
    CPLString osCommand;
    PGresult * poResult = NULL;

    if (poSrcDS->pszWhere == NULL) {
        osCommand.Printf("insert into %s.%s (%s) (select %s from %s.%s)",
            pszSchema, pszTable, pszColumn, poSrcDS->pszColumn, 
            poSrcDS->pszSchema, poSrcDS->pszTable);
    }
    else {
        osCommand.Printf("insert into %s.%s (%s) (select %s from %s.%s where %s)",
            pszSchema, pszTable, pszColumn, poSrcDS->pszColumn, 
            poSrcDS->pszSchema, poSrcDS->pszTable, poSrcDS->pszWhere);
    }

    CPLDebug("PostGIS_Raster", "PostGISRasterDataset::InsertRaster(): Query = %s",
        osCommand.c_str());

    poResult = PQexec(poConn, osCommand.c_str());
    if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {

        CPLError(CE_Failure, CPLE_AppDefined,
                "Error inserting raster: %s",
                PQerrorMessage(poConn));
        if (poResult != NULL)
            PQclear(poResult);

        return false;
    }

    PQclear(poResult);

    return true;
}

/*********************************************************
 * \brief Delete a PostGIS Raster dataset. 
 *********************************************************/
CPLErr
PostGISRasterDataset::Delete(const char* pszFilename) 
{
    char* pszSchema = NULL;
    char* pszTable = NULL;
    char* pszColumn = NULL;
    char* pszWhere = NULL;
    GBool bBrowseDatabase;
    char* pszConnectionString = NULL;
    int nMode;
    PGconn * poConn = NULL;
    PGresult * poResult = NULL;
    CPLString osCommand;
    CPLErr nError = CE_Failure;

    // Check connection string
    if (pszFilename == NULL ||
        !EQUALN(pszFilename, "PG:", 3)) { 
        /**
         * The connection string provided is not a valid connection 
         * string.
         */
        CPLError( CE_Failure, CPLE_NotSupported, 
            "PostGIS Raster driver was unable to parse the provided "
            "connection string. Nothing was deleted." );
        return CE_Failure;
    }

    poConn = GetConnection(pszFilename, &pszConnectionString, 
        &pszSchema, &pszTable, &pszColumn, &pszWhere,
        &nMode, &bBrowseDatabase);
    if (poConn == NULL) {
        CPLFree(pszConnectionString);
        CPLFree(pszSchema);
        CPLFree(pszTable);
        CPLFree(pszColumn);
        CPLFree(pszWhere);

        return CE_Failure;
    }

    // begin transaction
    poResult = PQexec(poConn, "begin");
    if (poResult == NULL ||
        PQresultStatus(poResult) != PGRES_COMMAND_OK) {
        CPLError(CE_Failure, CPLE_AppDefined,
            "Error beginning database transaction: %s",
            PQerrorMessage(poConn));

        // set nMode to NO_MODE to avoid any further processing
        nMode = NO_MODE;
    }

    PQclear(poResult);

    if ( nMode == ONE_RASTER_PER_TABLE || 
        (nMode == ONE_RASTER_PER_ROW && pszWhere == NULL)) {
        // without a where clause, this delete command shall delete
        // all subdatasets, even if the mode is ONE_RASTER_PER_ROW

        // drop table <schema>.<table>;
        osCommand.Printf("drop table %s.%s", pszSchema, pszTable);
        poResult = PQexec(poConn, osCommand.c_str());
        if (poResult == NULL || 
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Couldn't drop the table %s.%s: %s",
                    pszSchema, pszTable, PQerrorMessage(poConn));
        }
        else {
            PQclear(poResult);
            nError = CE_None;
        }
    }
    else if (nMode == ONE_RASTER_PER_ROW) {

        // delete from <schema>.<table> where <where>
        osCommand.Printf("delete from %s.%s where %s", pszSchema, 
            pszTable, pszWhere);
        poResult = PQexec(poConn, osCommand.c_str());
        if (poResult == NULL || 
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Couldn't delete records from the table %s.%s: %s",
                    pszSchema, pszTable, PQerrorMessage(poConn));
        }
        else {
            PQclear(poResult);
            nError = CE_None;
        }
    }

    // if mode == NO_MODE, the begin transaction above did not complete,
    // so no commit is necessary
    if (nMode != NO_MODE) {
        poResult = PQexec(poConn, "commit");
        if (poResult == NULL ||
            PQresultStatus(poResult) != PGRES_COMMAND_OK) {
            CPLError(CE_Failure, CPLE_AppDefined,
                "Error committing database transaction: %s",
                PQerrorMessage(poConn));

            nError = CE_Failure;
        }
    }

    if (poResult)
        PQclear(poResult);
    if (pszSchema)
        CPLFree(pszSchema);
    if (pszTable)
        CPLFree(pszTable);
    if (pszColumn)
        CPLFree(pszColumn);
    if (pszWhere)
        CPLFree(pszWhere);

    // clean up connection string
    CPLFree(pszConnectionString);

    return nError;
}

/************************************************************************/
/*                          GDALRegister_PostGISRaster()                */

/************************************************************************/
void GDALRegister_PostGISRaster() {
    GDALDriver *poDriver;

    if (GDALGetDriverByName("PostGISRaster") == NULL) {
        poDriver = new PostGISRasterDriver();

        poDriver->SetDescription("PostGISRaster");
        poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                "PostGIS Raster driver");

        poDriver->pfnOpen = PostGISRasterDataset::Open;
        poDriver->pfnCreateCopy = PostGISRasterDataset::CreateCopy;
        poDriver->pfnDelete = PostGISRasterDataset::Delete;

        GetGDALDriverManager()->RegisterDriver(poDriver);
    }
}
