<?php
/* Test mbcryptload -- PHP 8.4.23 (TS: php_mbcryptload_php84_ts.dll / NTS: php_mbcryptload_php84_nts.dll) */
mbcryptload_set_password("mbcrypt-test-2026");
require "calcul.encrypted.php";   // s'execute normalement
