# -*- coding: utf-8 -*-

"""
***************************************************************************
    spatialite_utils.py
    ---------------------
    Date                 : November 2015
    Copyright            : (C) 2015 by René-Luc Dhont
    Email                : volayaf at gmail dot com
***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************
"""

__author__ = 'René-Luc Dhont'
__date__ = 'November 2015'
__copyright__ = '(C) 2015, René-Luc Dhont'

# This will get replaced with a git SHA1 when you do a git archive

__revision__ = '$Format:%H$'

from pyspatialite import dbapi2 as sqlite
import re


class DbError(Exception):

    def __init__(self, message, query=None):
        # Save error. funny that the variables are in utf-8
        self.message = unicode(message, 'utf-8')
        self.query = (unicode(query, 'utf-8') if query is not None else None)

    def __str__(self):
        return 'MESSAGE: %s\nQUERY: %s' % (self.message, self.query)


class GeoDB:

    def __init__(self, uri=None):
        self.uri = uri
        self.dbname = uri.database()

        try:
            self.con = sqlite.connect(self.con_info())

        except (sqlite.InterfaceError, sqlite.OperationalError) as e:
            raise DbError(e.message)

        self.has_spatialite = self.check_spatialite()
        if not self.has_spatialite:
            self.has_spatialite = self.init_spatialite()

    def con_info(self):
        return unicode(self.dbname)

    def init_spatialite(self):
        # Get spatialite version
        c = self.con.cursor()
        try:
            self._exec_sql(c, u'SELECT spatialite_version()')
            rep = c.fetchall()
            v = [int(x) if x.isdigit() else x for x in re.findall("\d+|[a-zA-Z]+", rep[0][0])]

            # Add spatialite support
            if v >= [4, 1, 0]:
                # 4.1 and above
                sql = "SELECT initspatialmetadata(1)"
            else:
                # Under 4.1
                sql = "SELECT initspatialmetadata()"
            self._exec_sql_and_commit(sql)
        except:
            return False
        finally:
            self.con.close()

        try:
            self.con = sqlite.connect(self.con_info())

        except (sqlite.InterfaceError, sqlite.OperationalError) as e:
            raise DbError(e.message)

        return self.check_spatialite()

    def check_spatialite(self):
        try:
            c = self.con.cursor()
            self._exec_sql(c, u"SELECT CheckSpatialMetaData()")
            v = c.fetchone()[0]
            self.has_geometry_columns = v == 1 or v == 3
            self.has_spatialite4 = v == 3
        except Exception as e:
            self.has_geometry_columns = False
            self.has_spatialite4 = False

        self.has_geometry_columns_access = self.has_geometry_columns
        return self.has_geometry_columns

    def _exec_sql(self, cursor, sql):
        try:
            cursor.execute(sql)
        except (sqlite.Error, sqlite.ProgrammingError, sqlite.Warning, sqlite.InterfaceError, sqlite.OperationalError) as e:
            raise DbError(e.message, sql)

    def _exec_sql_and_commit(self, sql):
        """Tries to execute and commit some action, on error it rolls
        back the change.
        """

        try:
            c = self.con.cursor()
            self._exec_sql(c, sql)
            self.con.commit()
        except DbError as e:
            self.con.rollback()
            raise
