/*
 * Copyright © 2021 Red Hat, Inc.
 * Author(s): David Cantrell <dcantrell@redhat.com>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see
 * <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <assert.h>
#include <err.h>
#include <rpm/header.h>
#include "queue.h"
#include "rpminspect.h"

/* For reporting */
static const char *specfile = NULL;

/*
 * Scan all dependencies and look for version values containing
 * unexpanded macros.  Anything found is reported as a failure.
 */
static bool have_unexpanded_macros(struct rpminspect *ri, const char *name, const char *arch, deprule_list_t *deprules)
{
    bool result = true;
    deprule_entry_t *entry = NULL;
    char *near = NULL;
    char *far = NULL;
    char *noun = NULL;
    const char *desc = NULL;
    char *r = NULL;
    struct result_params params;

    assert(ri != NULL);
    assert(name != NULL);
    assert(name != NULL);

    if (deprules == NULL) {
        return true;
    }

    /* initialize result parameters */
    init_result_params(&params);
    params.waiverauth = WAIVABLE_BY_ANYONE;
    params.header = NAME_RPMDEPS;
    params.remedy = REMEDY_RPMDEPS_MACROS;
    params.file = specfile;

    /* check all dependencies */
    TAILQ_FOREACH(entry, deprules, items) {
        if (entry->version == NULL) {
            continue;
        }

        /* find the macro beginning substring */
        near = far = strstr(entry->version, "%{");

        /* starting from there, find the macro ending substring */
        if (near != NULL) {
            far = strstr(entry->version, "}");
        }

        /* we have at least one macro, report */
        if (near != NULL && far != NULL) {
            desc = get_deprule_desc(entry->type);
            r = strdeprule(entry);

            xasprintf(&params.msg, _("Invalid looking %s dependency in the %s package on %s: %s"), desc, name, arch, r);
            params.severity = RESULT_BAD;
            params.verb = VERB_FAILED;
            xasprintf(&noun, _("'${FILE}' in %s on ${ARCH}"), name);
            params.file = r;
            params.noun = noun;
            params.arch = arch;
            add_result(ri, &params);
            free(params.msg);
            free(noun);
            free(r);

            result = false;
        }
    }

    return result;
}

/*
 * Verify the after build subpackages all carry explicit Requires:
 * dependencies for autogenerated shared library dependencies.  Also
 * make sure there are not multiple packages providing the same shared
 * library dependency.
 */
static bool check_explicit_lib_deps(struct rpminspect *ri, Header h, deprule_list_t *after_deps)
{
    bool result = true;
    const char *name = NULL;
    const char *arch = NULL;
    rpmpeer_entry_t *peer = NULL;
    rpmpeer_entry_t *potential_prov = NULL;
    deprule_entry_t *verify = NULL;
    deprule_entry_t *req = NULL;
    deprule_entry_t *prov = NULL;
    char *isareq = NULL;
    char *isaprov = NULL;
    char *multiples = NULL;
    char *r = NULL;
    char *noun = NULL;
    const char *pn = NULL;
    const char *pv = NULL;
    const char *pr = NULL;
    uint64_t epoch = 0;
    char *rulestr = NULL;
    bool found = false;
    struct result_params params;

    assert(ri != NULL);
    assert(h != NULL);

    name = headerGetString(h, RPMTAG_NAME);
    arch = get_rpm_header_arch(h);

    init_result_params(&params);
    params.waiverauth = WAIVABLE_BY_ANYONE;
    params.header = NAME_RPMDEPS;
    params.file = specfile;

    /* iterate over the deps of the after build peer */
    TAILQ_FOREACH(req, after_deps, items) {
        /* only looking at lib* Requires right now */
        if ((req->type != TYPE_REQUIRES) || !strprefix(req->requirement, SHARED_LIB_PREFIX)) {
            continue;
        }

        found = false;
        potential_prov = NULL;

        /* we have a lib Requires, find what subpackage Provides it */
        TAILQ_FOREACH(peer, ri->peers, items) {
            /* skip anything with no dependencies */
            if (peer->after_deprules == NULL || TAILQ_EMPTY(peer->after_deprules)) {
                continue;
            }

            /* check this peer's Provides */
            TAILQ_FOREACH(prov, peer->after_deprules, items) {
                /* skip the entry we're trying to match against */
                if (req == prov) {
                    continue;
                }

                /* only looking at Provides right now */
                if ((prov->type != TYPE_PROVIDES) || !strprefix(prov->requirement, SHARED_LIB_PREFIX)) {
                    continue;
                }

                pn = headerGetString(peer->after_hdr, RPMTAG_NAME);

                /* check that the Requires is explict and matches the Provides */
                if (!strcmp(req->requirement, prov->requirement)) {
                    /* a package is allowed to to Provide and Require the same thing */
                    /* otherwise we found the subpackage that Provides this explicit Requires */
                    potential_prov = peer;
                    req->providers = list_add(req->providers, pn);
                    found = true;
                } else if (strstr(req->requirement, "(") || strstr(prov->requirement, "(")) {
                    /*
                     * we may have a dependency such as:
                     *     Requires: %{name}-libs%{?_isa} = %{version}-%{release}'
                     * trim the '(x86-64)' or similar ISA substring for comparision purposes
                     */
                    isareq = strdup(req->requirement);
                    isareq[strcspn(isareq, "(")] = '\0';

                    isaprov = strdup(prov->requirement);
                    isaprov[strcspn(isaprov, "(")] = '\0';

                    if (!strcmp(isareq, isaprov)) {
                        potential_prov = peer;
                        req->providers = list_add(req->providers, pn);
                        found = true;
                    }

                    free(isareq);
                    free(isaprov);
                }
            }

            /* if we have a provider, get out of the loop */
            if (found) {
                break;
            }
        }

        /* now look for the explicit Requires of potential_prov */
        if (found && potential_prov) {
            /* prove yourself again */
            found = false;

            pn = headerGetString(potential_prov->after_hdr, RPMTAG_NAME);
            pv = headerGetString(potential_prov->after_hdr, RPMTAG_VERSION);
            pr = headerGetString(potential_prov->after_hdr, RPMTAG_RELEASE);

            /* the version-release or epoch:version-release string */
            epoch = headerGetNumber(potential_prov->after_hdr, RPMTAG_EPOCH);

            if (epoch > 0) {
                xasprintf(&r, "%ju:%s-%s", epoch, pv, pr);
            } else {
                xasprintf(&r, "%s-%s", pv, pr);
            }

            assert(r != NULL);

            TAILQ_FOREACH(verify, after_deps, items) {
                /* look only at explicit Requires right now */
                if ((verify->type != TYPE_REQUIRES) || strprefix(verify->requirement, SHARED_LIB_PREFIX)) {
                    continue;
                }

                if (!strcmp(verify->requirement, pn) && verify->operator == OP_EQUAL && !strcmp(verify->version, r)) {
                    found = true;
                    break;
                }
            }

            free(r);
        }

        /* report missing explicit package requires */
        if (!found && potential_prov) {
            r = strdeprule(req);
            pn = headerGetString(potential_prov->after_hdr, RPMTAG_NAME);

            if (epoch > 0) {
                rulestr = "%{epoch}:%{version}-%{release}";
                params.remedy = REMEDY_RPMDEPS_EXPLICIT_EPOCH;
            } else {
                rulestr = "%{version}-%{release}";
                params.remedy = REMEDY_RPMDEPS_EXPLICIT;
            }

            xasprintf(&params.msg, _("Subpackage %s on %s carries '%s' which comes from subpackage %s but does not carry an explicit package version requirement.  Please add 'Requires: %s = %s' to the spec file to avoid the need to test interoperability between various combinations of old and new subpackages."), name, arch, r, pn, pn, rulestr);
            xasprintf(&noun, _("missing 'Requires: ${FILE} = %s' in %s on ${ARCH}"), rulestr, name);

            params.severity = RESULT_VERIFY;
            params.noun = noun;
            params.verb = VERB_FAILED;
            params.file = pn;
            params.arch = arch;
            add_result(ri, &params);
            free(noun);
            free(params.msg);
            free(r);

            result = false;
        }

        /* check for multiple providers foreach Requires */
        if (list_len(req->providers) > 1) {
            r = strdeprule(req);
            multiples = list_to_string(req->providers, ", ");

            xasprintf(&params.msg, _("Multiple subpackages provide '%s': %s"), r, multiples);
            xasprintf(&noun, _("%s all provide '${FILE}' on ${ARCH}"), multiples);
            params.severity = RESULT_VERIFY;
            params.file = r;
            params.arch = arch;
            params.remedy = REMEDY_RPMDEPS_MULTIPLE;
            params.verb = VERB_FAILED;
            params.noun = noun;
            add_result(ri, &params);
            free(params.msg);
            free(noun);
            free(r);

            free(multiples);
            result = false;
        }
    }

    return result;
}

/*
 * For packages with an Epoch > 0, check each dependency rule string
 * that uses the package version and release and ensure it comes
 * prefixed with the Epoch followed by a colon.
 */
static bool check_explicit_epoch(struct rpminspect *ri, Header h, deprule_list_t *afterdeps)
{
    bool result = true;
    const char *name = NULL;
    const char *arch = NULL;
    uint64_t epoch;
    char *verrel = NULL;
    char *epochprefix = NULL;
    deprule_entry_t *deprule = NULL;
    char *drs = NULL;
    char *noun = NULL;
    struct result_params params;

    assert(ri != NULL);
    assert(h != NULL);

    name = headerGetString(h, RPMTAG_NAME);
    arch = get_rpm_header_arch(h);
    epoch = headerGetNumber(h, RPMTAG_EPOCH);

    /* need deps to continue here */
    if (afterdeps == NULL || TAILQ_EMPTY(afterdeps)) {
        return true;
    }

    /* skip epoch values of 0 */
    if (epoch == 0) {
        return true;
    }

    /* set up result parameters */
    init_result_params(&params);
    params.header = NAME_RPMDEPS;
    params.file = specfile;

    if (is_rebase(ri)) {
        params.waiverauth = NOT_WAIVABLE;
        params.severity = RESULT_INFO;
    } else {
        params.waiverauth = WAIVABLE_BY_ANYONE;
        params.severity = RESULT_BAD;
    }

    /*
     * check every deprule here that uses the package version and
     * release to ensure it is prefixed with the epoch
     */
    xasprintf(&verrel, "%s-%s", headerGetString(h, RPMTAG_VERSION), headerGetString(h, RPMTAG_RELEASE));
    assert(verrel != NULL);
    xasprintf(&epochprefix, "%ju:", epoch);
    assert(epochprefix != NULL);

    TAILQ_FOREACH(deprule, afterdeps, items) {
        if (deprule->version == NULL) {
            continue;
        }

        if (strsuffix(deprule->version, verrel) && !strprefix(deprule->version, epochprefix)) {
            drs = strdeprule(deprule);
            xasprintf(&params.msg, _("Missing epoch prefix on the version-release in '%s' for %s on %s"), drs, name, arch);
            xasprintf(&noun, _("'${FILE}' needs epoch in %s on ${ARCH}"), name);

            params.remedy = REMEDY_RPMDEPS_EPOCH;
            params.verb = VERB_FAILED;
            params.noun = noun;
            params.arch = arch;
            params.file = drs;

            add_result(ri, &params);
            free(params.msg);
            free(noun);
            free(drs);

            result = false;
        }
    }

    free(verrel);
    free(epochprefix);
    return result;
}

/*
 * Check if the deprule change is expected (e.g., automatic Provides).
 */
static bool expected_deprule_change(const bool rebase, const deprule_entry_t *deprule, const Header h, const rpmpeer_t *peers)
{
    bool r = false;
    bool found = false;
    char *req = NULL;
    char *vr = NULL;
    char *evr = NULL;
    rpmpeer_entry_t *peer = NULL;
    const char *name = NULL;
    const char *arch = NULL;
    const char *peerarch = NULL;
    const char *version = NULL;
    const char *release = NULL;
    uint64_t epoch = 0;

    assert(deprule != NULL);
    assert(h != NULL);

    /* skip source packages */
    if (headerIsSource(h)) {
        return true;
    }

    /* changes always expected in a rebase */
    if (rebase) {
        return true;
    }

    /* this package arch */
    arch = get_rpm_header_arch(h);

    /* trim any arch substrings from the name (e.g., "(x86-64)") */
    req = strdup(deprule->requirement);
    assert(req != NULL);

    if (strstr(req, "(")) {
        req[strcspn(req, "(")] = '\0';
    }

    /* see if this deprule requirement name matches a subpackage */
    TAILQ_FOREACH(peer, peers, items) {
        /* no after peer, ignore */
        if (peer->after_hdr == NULL) {
            continue;
        }

        /* skip source */
        if (headerIsSource(peer->after_hdr)) {
            continue;
        }

        /* match the arch and name */
        peerarch = get_rpm_header_arch(peer->after_hdr);
        name = headerGetString(peer->after_hdr, RPMTAG_NAME);

        if (!strcmp(arch, peerarch) && !strcmp(name, req)) {
            /* we found the subpackage, which is now 'peer' for code below */
            found = true;
            break;
        }
    }

    if (!found) {
        free(req);
        return false;
    }

    /* deprule version strings are a combo of the version-release or epoch:version-release */
    version = headerGetString(peer->after_hdr, RPMTAG_VERSION);
    release = headerGetString(peer->after_hdr, RPMTAG_RELEASE);
    epoch = headerGetNumber(peer->after_hdr, RPMTAG_EPOCH);
    xasprintf(&vr, "%s-%s", version, release);
    xasprintf(&evr, "%ju:%s-%s", epoch, version, release);

    /* determine if this is expected */
    if (deprule->operator == OP_EQUAL) {
        if (epoch > 0) {
            r = (evr && !strcmp(deprule->version, evr));
        } else {
            r = (vr && !strcmp(deprule->version, vr));
        }
    }

    free(req);
    free(vr);
    free(evr);

    return r;
}

/*
 * Main driver for the 'rpmdeps' inspection.
 */
bool inspect_rpmdeps(struct rpminspect *ri)
{
    bool result = true;
    bool rebase = false;
    rpmpeer_entry_t *peer = NULL;
    rpmfile_entry_t *file = NULL;
    deprule_entry_t *deprule = NULL;
    const char *name = NULL;
    const char *arch = NULL;
    char *drs = NULL;
    char *pdrs = NULL;
    char *noun = NULL;
    bool found = false;
    struct result_params params;

    assert(ri != NULL);

    /* are these builds a rebase? */
    rebase = is_rebase(ri);

    /* set up result parameters */
    init_result_params(&params);
    params.header = NAME_RPMDEPS;
    params.file = specfile;

    /*
     * for reporting, we need the name of the spec file from the SRPM
     *
     * NOTE: we only need this for reporting, so if we don't have a
     * spec file name, we will just adjust the reporting strings
     * later
     */
    TAILQ_FOREACH(peer, ri->peers, items) {
        if (!headerIsSource(peer->after_hdr)) {
            continue;
        }

        TAILQ_FOREACH(file, peer->after_files, items) {
            if (strsuffix(file->localpath, SPEC_FILENAME_EXTENSION)) {
                found = true;
                specfile = file->localpath;
                break;
            }
        }

        if (found) {
            break;
        }
    }

    /* for cases where the job lacks the SRPM, just say spec file */
    if (specfile == NULL) {
        specfile = _("spec file");
    }

    params.file = specfile;

    /*
     * first pass gathers deps and performs simple checks
     */
    TAILQ_FOREACH(peer, ri->peers, items) {
        /* Gather deprules */
        if (peer->before_hdr && peer->before_deprules == NULL) {
            peer->before_deprules = gather_deprules(peer->before_hdr);
        }

        if (peer->after_deprules == NULL) {
            peer->after_deprules = gather_deprules(peer->after_hdr);
        }

        /* Peer up the before and after deps */
        find_deprule_peers(peer->before_deprules, peer->after_deprules);

        /* Name and arch of this peer */
        name = headerGetString(peer->after_hdr, RPMTAG_NAME);
        arch = get_rpm_header_arch(peer->after_hdr);

        /* Check for unexpanded macros in the version fields of dependencies */
        if (!have_unexpanded_macros(ri, name, arch, peer->after_deprules)) {
            result = false;
        }
    }

    /*
     * the second pass performs more complex checks
     */
    TAILQ_FOREACH(peer, ri->peers, items) {
        /* Check for required explicit 'lib' dependencies */
        if (!check_explicit_lib_deps(ri, peer->after_hdr, peer->after_deprules)) {
            result = false;
        }

        /* Check that packages defining an Epoch > 0 use it for Provides */
        if (!check_explicit_epoch(ri, peer->after_hdr, peer->after_deprules)) {
            result = false;
        }
    }

    /* report dependency findings between the before and after build */
    if (ri->before && ri->after) {
        TAILQ_FOREACH(peer, ri->peers, items) {
            /* Name and arch of this peer */
            name = headerGetString(peer->after_hdr, RPMTAG_NAME);
            arch = get_rpm_header_arch(peer->after_hdr);

            if (peer->after_deprules) {
                TAILQ_FOREACH(deprule, peer->after_deprules, items) {
                    /* reporting level */
                    if (rebase) {
                        params.waiverauth = NOT_WAIVABLE;
                        params.severity = RESULT_INFO;
                    } else {
                        params.waiverauth = WAIVABLE_BY_ANYONE;
                        params.severity = RESULT_VERIFY;
                    }

                    /* use shorter variable names in the if expressions */
                    drs = strdeprule(deprule);

                    if (deprule->peer_deprule) {
                        pdrs = strdeprule(deprule->peer_deprule);
                    }

                    /* determine what to report */
                    if (deprule->peer_deprule == NULL ) {
                        if (!strcmp(arch, SRPM_ARCH_NAME)) {
                            xasprintf(&params.msg, _("Gained '%s' in source package %s"), drs, name);
                        } else {
                            xasprintf(&params.msg, _("Gained '%s' in subpackage %s on %s"), drs, name, arch);
                        }

                        xasprintf(&noun, _("'${FILE}' in %s on ${ARCH}"), name);
                        params.remedy = REMEDY_RPMDEPS_GAINED;
                        params.verb = VERB_ADDED;
                    } else if (deprules_match(deprule, deprule->peer_deprule)) {
                        if (!strcmp(arch, SRPM_ARCH_NAME)) {
                            xasprintf(&params.msg, _("Retained '%s' in source package %s"), drs, name);
                        } else {
                            xasprintf(&params.msg, _("Retained '%s' in subpackage %s on %s"), drs, name, arch);
                        }

                        xasprintf(&noun, _("'${FILE}' in %s on ${ARCH}"), name);
                        params.remedy = NULL;
                        params.verb = VERB_OK;
                        params.waiverauth = NOT_WAIVABLE;
                        params.severity = RESULT_INFO;
                    } else {
                        if (!strcmp(arch, SRPM_ARCH_NAME)) {
                            xasprintf(&params.msg, _("Changed '%s' to '%s' in source package %s"), pdrs, drs, name);
                        } else {
                            xasprintf(&params.msg, _("Changed '%s' to '%s' in subpackage %s on %s"), pdrs, drs, name, arch);
                        }

                        xasprintf(&noun, _("'%s' became '${FILE}' in %s on ${ARCH}"), pdrs, name);
                        params.remedy = REMEDY_RPMDEPS_CHANGED;
                        params.verb = VERB_CHANGED;

                        if (expected_deprule_change(rebase, deprule, peer->after_hdr, ri->peers)) {
                            params.severity = RESULT_INFO;
                            params.waiverauth = NOT_WAIVABLE;
                            params.msg = strappend(params.msg, _("; this is expected"), NULL);
                        }
                    }

                    params.noun = noun;
                    params.arch = arch;
                    params.file = drs;
                    add_result(ri, &params);
                    free(params.msg);
                    free(noun);
                    free(drs);

                    if (deprule->peer_deprule) {
                        free(pdrs);
                    }

                    if (params.severity == RESULT_VERIFY) {
                        result = false;
                    }
                }
            }

            if (peer->before_deprules) {
                TAILQ_FOREACH(deprule, peer->before_deprules, items) {
                    if (deprule && deprule->peer_deprule == NULL) {
                        pdrs = strdeprule(deprule);

                        if (!strcmp(arch, SRPM_ARCH_NAME)) {
                            xasprintf(&params.msg, _("Lost '%s' in source package %s"), pdrs, name);
                        } else {
                            xasprintf(&params.msg, _("Lost '%s' in subpackage %s on %s"), pdrs, name, arch);
                        }

                        params.details = NULL;
                        params.remedy = REMEDY_RPMDEPS_LOST;
                        params.verb = VERB_REMOVED;
                        xasprintf(&noun, _("'${FILE}' in %s on ${ARCH}"), name);
                        params.noun = noun;
                        params.file = pdrs;
                        params.arch = arch;

                        if (rebase) {
                            params.waiverauth = NOT_WAIVABLE;
                            params.severity = RESULT_INFO;
                        } else {
                            params.waiverauth = WAIVABLE_BY_ANYONE;
                            params.severity = RESULT_VERIFY;
                        }

                        add_result(ri, &params);
                        free(params.msg);
                        free(noun);
                        free(pdrs);

                        if (params.severity == RESULT_VERIFY) {
                            result = false;
                        }
                    }
                }
            }
        }
    }

    /* if everything was fine, just say so */
    if (result) {
        init_result_params(&params);
        params.severity = RESULT_OK;
        params.waiverauth = NOT_WAIVABLE;
        params.header = NAME_RPMDEPS;
        params.verb = VERB_OK;
        add_result(ri, &params);
    }

    return result;
}
