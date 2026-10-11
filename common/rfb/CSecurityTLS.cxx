/*
 * Copyright (C) 2004 Red Hat Inc.
 * Copyright (C) 2005 Martin Koegler
 * Copyright (C) 2010 TigerVNC Team
 * Copyright (C) 2010 m-privacy GmbH
 * Copyright 2012-2025 Pierre Ossman for Cendio AB
 *    
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef HAVE_GNUTLS
#error "This header should not be compiled without HAVE_GNUTLS defined"
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef WIN32
#include <unistd.h>
#endif

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>
#include <core/xdgdirs.h>

#include <rfb/CSecurityTLS.h>
#include <rfb/CConnection.h>
#include <rfb/Exception.h>

#include <rdr/TLSException.h>
#include <rdr/TLSSocket.h>

#include <gnutls/x509.h>

using namespace rfb;

static const char* configdirfn(const char* fn);

core::StringParameter CSecurityTLS::X509CA(
  "X509CA",
  _("Path to the X.509 certificate for the trusted certificate "
    "authority"),
  configdirfn("x509_ca.pem"));
core::StringParameter CSecurityTLS::X509CRL(
  "X509CRL",
  _("Path to the X.509 certificate revocation list"),
  configdirfn("x509_crl.pem"));

core::StringParameter CSecurityTLS::X509ClientCert(
  "X509ClientCert",
  _("Path to the X.509 certificate to present to the server. If set, "
    "the server must have a certificate for TLS servers from the "
    "certificate authority in X509CA"),
  "");
core::StringParameter CSecurityTLS::X509ClientKey(
  "X509ClientKey",
  _("Path to the private key of the client certificate, if it is not "
    "in the same file"),
  "");

static core::LogWriter vlog("TLS");

static const char* configdirfn(const char* fn)
{
  static char full_path[PATH_MAX];
  const char* configdir;

  configdir = core::getvncconfigdir();
  if (configdir == nullptr)
    return "";

  snprintf(full_path, sizeof(full_path), "%s/%s", configdir, fn);
  return full_path;
}

CSecurityTLS::CSecurityTLS(CConnection* cc_, bool _anon)
  : CSecurity(cc_), session(nullptr),
    anon_cred(nullptr), cert_cred(nullptr),
    anon(_anon), tlssock(nullptr),
    rawis(nullptr), rawos(nullptr)
{
  int err = gnutls_global_init();
  if (err != GNUTLS_E_SUCCESS)
    throw rdr::tls_error(_("Failed to initialize GnuTLS"), err);
}

void CSecurityTLS::shutdown()
{
  if (tlssock)
    tlssock->shutdown();

  if (anon_cred) {
    gnutls_anon_free_client_credentials(anon_cred);
    anon_cred = nullptr;
  }

  if (cert_cred) {
    gnutls_certificate_free_credentials(cert_cred);
    cert_cred = nullptr;
  }

  if (rawis && rawos) {
    cc->setStreams(rawis, rawos);
    rawis = nullptr;
    rawos = nullptr;
  }

  if (tlssock) {
    delete tlssock;
    tlssock = nullptr;
  }

  if (session) {
    gnutls_deinit(session);
    session = nullptr;
  }
}


CSecurityTLS::~CSecurityTLS()
{
  shutdown();

  gnutls_global_deinit();
}

bool CSecurityTLS::processMsg()
{
  rdr::InStream* is = cc->getInStream();
  rdr::OutStream* os = cc->getOutStream();
  client = cc;

  if (!session) {
    int ret;

    if (!is->hasData(1))
      return false;

    if (is->readU8() == 0)
      throw protocol_error(
        _("Server failed to initialize TLS session"));

    ret = gnutls_init(&session, GNUTLS_CLIENT);
    if (ret != GNUTLS_E_SUCCESS)
      throw rdr::tls_error(_("Failed to initialize GnuTLS session"),
                           ret);

    ret = gnutls_set_default_priority(session);
    if (ret != GNUTLS_E_SUCCESS)
      throw rdr::tls_error(_("Failed to configure GnuTLS priority"),
                           ret);

    setParam();

    tlssock = new rdr::TLSSocket(is, os, session);

    rawis = is;
    rawos = os;
  }

  try {
    if (!tlssock->handshake())
      return false;
  } catch (std::exception&) {
    shutdown();
    throw;
  }

  vlog.debug("TLS handshake completed with %s",
             gnutls_session_get_desc(session));

  checkSession();

  cc->setStreams(&tlssock->inStream(), &tlssock->outStream());

  return true;
}

void CSecurityTLS::setParam()
{
  static const char kx_anon_priority[] = "+ANON-ECDH:+ANON-DH";

  int ret;

  // Custom priority string specified?
  if (strcmp(Security::GnuTLSPriority, "") != 0) {
    std::string prio;
    const char *err;

    prio = (const char*)Security::GnuTLSPriority;
    if (anon) {
      prio += ":";
      prio += kx_anon_priority;
    }

    ret = gnutls_priority_set_direct(session, prio.c_str(), &err);
    if (ret != GNUTLS_E_SUCCESS) {
      if (ret == GNUTLS_E_INVALID_REQUEST)
        vlog.error(_("Syntax error in GnuTLS priority string: %s"),
                   err);
      throw rdr::tls_error(_("Failed to configure GnuTLS priority"),
                           ret);
    }
  } else if (anon) {
    const char *err;

#if GNUTLS_VERSION_NUMBER >= 0x030603
    ret = gnutls_set_default_priority_append(session, kx_anon_priority, &err, 0);
    if (ret != GNUTLS_E_SUCCESS) {
      if (ret == GNUTLS_E_INVALID_REQUEST)
        vlog.error(_("Syntax error in GnuTLS priority string: %s"),
                   err);
      throw rdr::tls_error(_("Failed to configure GnuTLS priority"),
                           ret);
    }
#else
    std::string prio;

    // We don't know what the system default priority is, so we guess
    // it's what upstream GnuTLS has
    prio = "NORMAL";
    prio += ":";
    prio += kx_anon_priority;

    ret = gnutls_priority_set_direct(session, prio.c_str(), &err);
    if (ret != GNUTLS_E_SUCCESS) {
      if (ret == GNUTLS_E_INVALID_REQUEST)
        vlog.error(_("Syntax error in GnuTLS priority string: %s"),
                   err);
      throw rdr::tls_error(_("Failed to configure GnuTLS priority"),
                           ret);
    }
#endif
  }

  if (anon) {
    ret = gnutls_anon_allocate_client_credentials(&anon_cred);
    if (ret != GNUTLS_E_SUCCESS)
      throw rdr::tls_error("gnutls_anon_allocate_client_credentials()", ret);

    ret = gnutls_credentials_set(session, GNUTLS_CRD_ANON, anon_cred);
    if (ret != GNUTLS_E_SUCCESS)
      throw rdr::tls_error("gnutls_credentials_set()", ret);

    vlog.debug("Anonymous session has been set");
  } else {
    const char* hostname;
    size_t len;
    bool valid;

    ret = gnutls_certificate_allocate_credentials(&cert_cred);
    if (ret != GNUTLS_E_SUCCESS)
      throw rdr::tls_error("gnutls_certificate_allocate_credentials()", ret);

    if (useClientCert()) {
      const char* keyFile;
      struct stat st;

      // As with OpenVPN, only our own authority vouches for servers:
      // no system trust store, no exceptions, and errors are fatal
      ret = gnutls_certificate_set_x509_trust_file(cert_cred, X509CA,
                                                   GNUTLS_X509_FMT_PEM);
      if (ret < 1)
        throw rdr::tls_error(
          _("Failed to load the user specified certificate authority"),
          ret < 0 ? ret : GNUTLS_E_NO_CERTIFICATE_FOUND);

      if (stat(X509CRL, &st) == 0) {
        ret = gnutls_certificate_set_x509_crl_file(cert_cred, X509CRL,
                                                   GNUTLS_X509_FMT_PEM);
        if (ret < 0)
          throw rdr::tls_error(
            _("Failed to load the user specified certificate "
              "revocation list"), ret);
      }

      keyFile = X509ClientKey;
      if (strcmp(keyFile, "") == 0)
        keyFile = X509ClientCert;
      ret = gnutls_certificate_set_x509_key_file(cert_cred,
                                                 X509ClientCert,
                                                 keyFile,
                                                 GNUTLS_X509_FMT_PEM);
      if (ret != GNUTLS_E_SUCCESS)
        throw rdr::tls_error(
          _("Failed to load the client certificate and private key"),
          ret);
    } else {
      if (gnutls_certificate_set_x509_system_trust(cert_cred) < 1)
        vlog.error(_("Failed to load the system certificate trust store"));

      if (gnutls_certificate_set_x509_trust_file(cert_cred, X509CA, GNUTLS_X509_FMT_PEM) < 0)
        vlog.error(_("Failed to load the user specified certificate authority"));

      if (gnutls_certificate_set_x509_crl_file(cert_cred, X509CRL, GNUTLS_X509_FMT_PEM) < 0)
        vlog.error(_("Failed to load the user specified certificate revocation list"));
    }

    ret = gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, cert_cred);
    if (ret != GNUTLS_E_SUCCESS)
      throw rdr::tls_error("gnutls_credentials_set()", ret);

    // Only DNS hostnames are allowed, and some servers will reject the
    // connection if we provide anything else (e.g. an IPv6 address)
    hostname = client->getServerName();
    len = strlen(hostname);
    valid = true;
    for (size_t i = 0; i < len; i++) {
      if (!isalnum(hostname[i]) && hostname[i] != '.')
        valid = false;
    }

    if (valid) {
      if (gnutls_server_name_set(session, GNUTLS_NAME_DNS,
                                 client->getServerName(),
                                 strlen(client->getServerName())) != GNUTLS_E_SUCCESS)
        vlog.error(_("Failed to configure the server name for TLS "
                     "handshake"));
    }

    vlog.debug("X509 session has been set");
  }
}

bool CSecurityTLS::useClientCert()
{
  return strcmp(X509ClientCert, "") != 0;
}

void CSecurityTLS::checkSession()
{
  gnutls_typed_vdata_st data[2];
  unsigned int elements;
  unsigned int status;

  const gnutls_datum_t *cert_list;
  unsigned int cert_list_size = 0;
  int err;

  if (anon)
    return;

  if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
    gnutls_alert_send(session, GNUTLS_AL_FATAL,
                      GNUTLS_A_UNSUPPORTED_CERTIFICATE);
    throw protocol_error(_("Unsupported certificate type"));
  }

  // A certificate whose extended key usage leaves out TLS servers is
  // not one for servers, e.g. a client certificate of the same
  // authority
  elements = 0;
  data[elements].type = GNUTLS_DT_DNS_HOSTNAME;
  data[elements].data = (unsigned char*)client->getServerName();
  data[elements].size = 0;
  elements++;
  data[elements].type = GNUTLS_DT_KEY_PURPOSE_OID;
  data[elements].data = (unsigned char*)GNUTLS_KP_TLS_WWW_SERVER;
  data[elements].size = 0;
  elements++;

  err = gnutls_certificate_verify_peers(session, data, elements,
                                        &status);
  if (err != 0) {
    vlog.error(_("Server certificate verification failed: %s"),
               gnutls_strerror(err));
    gnutls_alert_send_appropriate(session, err);
    throw rdr::tls_error(_("Server certificate verification failed"),
                         err);
  }

  cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
  if (!cert_list_size) {
    gnutls_alert_send(session, GNUTLS_AL_FATAL,
                      GNUTLS_A_UNSUPPORTED_CERTIFICATE);
    throw protocol_error(_("Empty certificate chain"));
  }

  if (useClientCert()) {
    // Not even a certificate without extended key usage will do
    if (status == 0 && !hasPurpose(&cert_list[0],
                                   GNUTLS_KP_TLS_WWW_SERVER))
      status = GNUTLS_CERT_INVALID | GNUTLS_CERT_PURPOSE_MISMATCH;

    if (status != 0) {
      gnutls_datum_t status_str;
      std::string reason;

      if (gnutls_certificate_verification_status_print(
            status, GNUTLS_CRT_X509, &status_str, 0) == GNUTLS_E_SUCCESS) {
        reason = (const char*)status_str.data;
        gnutls_free(status_str.data);
      }
      gnutls_alert_send(session, GNUTLS_AL_FATAL,
                        GNUTLS_A_BAD_CERTIFICATE);
      throw protocol_error(core::format(
        _("Invalid server certificate: %s"), reason.c_str()));
    }
  }

  /* Everything green? */
  if (status == 0)
    return;

  try {
    /* Process only server's certificate, not issuer's certificate */
    if (!cc->verifyCertificate(status, cert_list[0].data,
                              cert_list[0].size)) {
      gnutls_alert_send(session, GNUTLS_AL_FATAL,
                        GNUTLS_A_BAD_CERTIFICATE);
      throw auth_cancelled();
    }
  } catch(rdr::tls_error& e) {
    gnutls_alert_send_appropriate(session, e.err);
    throw;
  }
}

bool CSecurityTLS::hasPurpose(const gnutls_datum_t* cert,
                              const char* oid)
{
  gnutls_x509_crt_t crt;
  bool found;

  if (gnutls_x509_crt_init(&crt) != GNUTLS_E_SUCCESS)
    return false;
  if (gnutls_x509_crt_import(crt, cert,
                             GNUTLS_X509_FMT_DER) != GNUTLS_E_SUCCESS) {
    gnutls_x509_crt_deinit(crt);
    return false;
  }

  found = false;
  for (unsigned int i = 0; !found; i++) {
    char buf[128];
    size_t size = sizeof(buf);
    int err;

    err = gnutls_x509_crt_get_key_purpose_oid(crt, i, buf, &size,
                                              nullptr);
    if (err == GNUTLS_E_SHORT_MEMORY_BUFFER)
      continue;
    if (err != GNUTLS_E_SUCCESS)
      break;
    found = strcmp(buf, oid) == 0;
  }

  gnutls_x509_crt_deinit(crt);
  return found;
}
