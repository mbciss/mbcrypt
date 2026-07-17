<?php
/* Test mbcryptload -- PHP 8.3.16 (TS: php_mbcryptload_php83_ts.dll / NTS: php_mbcryptload_php83_nts.dll) */
mbcryptload_set_password("mbcrypt-test-2026");
require "calcul.encrypted.php";   // s'execute normalement
