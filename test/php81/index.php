<?php
/* Test mbcryptload -- PHP 8.1.10 (TS: php_mbcryptload_php81_ts.dll / NTS: php_mbcryptload_php81_nts.dll) */
mbcryptload_set_password("mbcrypt-test-2026");
require "calcul.encrypted.php";   // s'execute normalement
