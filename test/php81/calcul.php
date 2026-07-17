<?php
/**
 * Calculs mathematiques avances
 * Fibonacci iteratif, nombres premiers (crible d'Eratosthene),
 * factorielle, puissance modulaire rapide, moyenne/variance/ecart-type.
 */

/* --- Fibonacci iteratif jusqu'a N termes --- */
function fibonacci(int $n): array
{
    if ($n <= 0) return [];
    if ($n === 1) return [0];

    $seq = [0, 1];
    for ($i = 2; $i < $n; $i++) {
        $seq[] = $seq[$i - 1] + $seq[$i - 2];
    }
    return $seq;
}

/* --- Crible d'Eratosthene jusqu'a $limit --- */
function sieve_of_eratosthenes(int $limit): array
{
    if ($limit < 2) return [];

    $is_prime = array_fill(0, $limit + 1, true);
    $is_prime[0] = $is_prime[1] = false;

    for ($i = 2; $i * $i <= $limit; $i++) {
        if ($is_prime[$i]) {
            for ($j = $i * $i; $j <= $limit; $j += $i) {
                $is_prime[$j] = false;
            }
        }
    }

    return array_keys(array_filter($is_prime));
}

/* --- Factorielle recursive --- */
function factorielle(int $n): string
{
    // Utilise bcmath pour les grands nombres
    if ($n <= 1) return '1';
    return bcmul((string)$n, factorielle($n - 1));
}

/* --- Exponentiation modulaire rapide (base^exp mod m) --- */
function pow_mod(int $base, int $exp, int $mod): int
{
    $result = 1;
    $base   = $base % $mod;

    while ($exp > 0) {
        if ($exp % 2 === 1) {
            $result = ($result * $base) % $mod;
        }
        $exp  = intdiv($exp, 2);
        $base = ($base * $base) % $mod;
    }
    return $result;
}

/* --- Statistiques : moyenne, variance, ecart-type --- */
function statistiques(array $data): array
{
    $n = count($data);
    if ($n === 0) return [];

    $moyenne  = array_sum($data) / $n;
    $variance = array_sum(array_map(fn($x) => ($x - $moyenne) ** 2, $data)) / $n;

    return [
        'n'           => $n,
        'min'         => min($data),
        'max'         => max($data),
        'moyenne'     => round($moyenne, 6),
        'variance'    => round($variance, 6),
        'ecart_type'  => round(sqrt($variance), 6),
    ];
}

/* ================================================================
   Execution et affichage des resultats
   ================================================================ */

echo "=== FICHIER NON CHIFFRE ===\n\n";

// Fibonacci
$fib = fibonacci(15);
echo "Fibonacci (15 termes) :\n";
echo "  " . implode(', ', $fib) . "\n\n";

// Nombres premiers
$primes = sieve_of_eratosthenes(100);
echo "Nombres premiers jusqu'a 100 (" . count($primes) . " trouves) :\n";
echo "  " . implode(', ', $primes) . "\n\n";

// Factorielle
$n_fact = 20;
echo "Factorielle de $n_fact :\n";
echo "  " . factorielle($n_fact) . "\n\n";

// Puissance modulaire
$base = 2; $exp = 100; $mod = 1000000007;
$result = pow_mod($base, $exp, $mod);
echo "Puissance modulaire : $base^$exp mod $mod\n";
echo "  = $result\n\n";

// Statistiques
$dataset = [12, 45, 7, 23, 89, 34, 56, 11, 78, 90, 3, 67, 42, 19, 55];
$stats   = statistiques($dataset);
echo "Statistiques du jeu de donnees :\n";
echo "  Donnees     : " . implode(', ', $dataset) . "\n";
foreach ($stats as $key => $val) {
    echo "  $key : $val\n";
}

echo "\nOK - calcul termine.\n";
