<?php
/* Test mbcryptload -- PHP 8.2.15 (TS: php_mbcryptload_php82_ts.dll / NTS: php_mbcryptload_php82_nts.dll) */
mbcryptload_set_password("mbcrypt-test-2026");
require "calcul.encrypted.php";   // s'execute normalement
