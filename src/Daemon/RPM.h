/*
    RPM.h - header file for rpm database
              - it implements query for local rpm database

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#ifndef RPM_H_
#define RPM_H_

#include <rpm/rpmcli.h>
#include <rpm/rpmts.h>
#include <rpm/rpmdb.h>
#include <rpm/rpmpgp.h>
#include "abrt_types.h"

/**
 * A class. It is used for additional checks of package, which contains
 * crashed application.
 */
class CRPM
{
    private:
        /**
         * A context for librpm library.
         */
        poptContext m_poptContext;
        /**
         * A set, which contains finger prints.
         */
        set_string_t m_setFingerprints;

    public:
        /**
         * A constructior.
         */
        CRPM();
        /**
         * A destructor.
         */
        ~CRPM();
        /**
         * A method, which loads one GPG public key.
         * @param pFileName A path to the public key.
         */
        void LoadOpenGPGPublicKey(const char* pFileName);
        /**
         * A method, which checks if package's finger print is valid.
         * @param pPackage A package name.
         */
        bool CheckFingerprint(const char* pPackage);
};

/**
 * Checks if an application is modified by third party.
 * @param pPackage A package name. The package contains the application.
 * @param pPath A path to the application.
 */
bool CheckHash(const char* pPackage, const char* pPath);
/**
 * Gets a package description.
 * @param pPackage A package name.
 * @return A package description.
 */
char* rpm_get_description(const char* pkg);
/**
 * Gets a package name. This package contains particular
 * file. If the file doesn't belong to any package, empty string is
 * returned.
 * @param pFileName A file name.
 * @return A package name (malloced string)
 */
char* rpm_get_package_nvr(const char* filename);
/**
 * Finds a main package for given file. This package contains particular
 * file. If the file doesn't belong to any package, empty string is
 * returned.
 * @param pFileName A file name.
 * @return a malloc'ed package name. Need to be freed.
 */
char* rpm_get_component(const char* filename);

#endif
