#!/usr/bin/env python3

import base64
import os
import random
import string
import dns.tsigkeyring

import dnstest.server

class Tsig(object):
    '''TSIG key generator'''

    algs = {
        "hmac-md5":    16,
        "hmac-sha1":   20,
        "hmac-sha224": 28,
        "hmac-sha256": 32,
        "hmac-sha384": 48,
        "hmac-sha512": 64
    }

    vocabulary = string.ascii_uppercase + string.ascii_lowercase + \
                 string.digits

    def __init__(self, alg=None):
        nlabels = random.randint(1, 10)

        self.name = ""
        for i in range(nlabels):
            label_len = random.randint(1, 63)

            # Check for maximal dname length (255 B = max fqdn in wire).
            # 255 = 1 leading byte + 253 + 1 trailing byte.
            if len(self.name) + 1 + label_len > 253:
                break

            # Add label separator.
            if i > 0:
                self.name += "."

            self.name += "".join(random.choice(Tsig.vocabulary)
                         for x in range(label_len))

        if alg and alg not in Tsig.algs:
            raise Exception("Unsupported TSIG algorithm %s" % alg)

        self.alg = alg if alg else random.choice(list(Tsig.algs.keys()))

        self.key = base64.b64encode(os.urandom(Tsig.algs[self.alg])). \
                   decode('ascii')

        # TSIG preparation for pythondns utils.
        if self.alg == "hmac-md5":
            alg = "hmac-md5.sig-alg.reg.int"
        else:
            alg = self.alg

        key = dns.tsigkeyring.from_text({
            self.name: self.key
        })
        self.key_params = dict(keyname=self.name, keyalgorithm=alg, keyring=key)

    def dump(self, filename):
        s = dnstest.server.BindConf()

        s.begin("key", self.name)
        s.item("algorithm", self.alg)
        s.item_str("secret", self.key)
        s.end()

        file = open(filename, mode="w")
        file.write(s.conf)
        file.close()
