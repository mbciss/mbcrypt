<?php
/* Test mbcryptload -- PHP 8.0.30 (TS: php_mbcryptload_php80_ts.dll / NTS: php_mbcryptload_php80_nts.dll) */
mbcryptload_set_password("mbcrypt-test-2026");
require "calcul.encrypted.php";   // s'execute normalement
