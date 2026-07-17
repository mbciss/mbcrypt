<?php
/* Test mbcryptload -- PHP 8.5.8 (TS: php_mbcryptload_php85_ts.dll / NTS: php_mbcryptload_php85_nts.dll) */
mbcryptload_set_password("mbcrypt-test-2026");
require "calcul.encrypted.php";   // s'execute normalement
