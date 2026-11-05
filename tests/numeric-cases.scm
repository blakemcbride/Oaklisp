;;; numeric-cases.scm -- random numeric cases with their answers, by
;;; Guile, for numeric-check.oak to compare Oaklisp against.
;;;
;;;   guile numeric-cases.scm SEED COUNT OUTFILE
;;;
;;; Writes COUNT cases per operation, each a line (OP (ARG ...) ANSWER),
;;; in syntax both Lisps read.  Guile's integers and rationals are
;;; exact, its QUOTIENT, REMAINDER, MODULO, ROUND (ties to even), GCD,
;;; LCM and EXPT mean what Oaklisp's do, and its LOGAND and friends are
;;; two's complement on integers of any size, as Oaklisp's BIT-AND and
;;; friends are.  The operands favour the places bugs live: zero and
;;; one, the edges of the fixnum range on either word size, powers of
;;; two and of the bignum digit bases (10^4, 10^9), and bignums of
;;; many digits.

(use-modules (ice-9 format))

(define args (cdr (command-line)))
(define seed (string->number (list-ref args 0)))
(define count (string->number (list-ref args 1)))
(define outfile (list-ref args 2))
(define st (seed->random-state seed))

(define (rnd n) (random n st))
(define (pick l) (list-ref l (rnd (length l))))
(define (sign x) (if (zero? (rnd 2)) x (- x)))

;; An integer from one of the classes, either sign.
(define (rand-int)
  (case (rnd 9)
    ((0) (pick '(0 1 -1 2 -2)))
    ((1) (sign (rnd 1000)))
    ((2) (sign (rnd (expt 2 30))))
    ((3) (sign (rnd (expt 2 62))))
    ;; The edges of the fixnum range: 30 bits and 62 bits.
    ((4) (+ (pick (list (expt 2 29) (- (expt 2 29)) (expt 2 61) (- (expt 2 61))))
	    (- (rnd 5) 2)))
    ;; Powers of 2 and of the digit bases, give or take one.
    ((5) (sign (+ (expt (pick '(2 10)) (rnd 130)) (- (rnd 3) 1))))
    ((6) (sign (+ (expt (pick '(10000 1000000000)) (+ 1 (rnd 6))) (- (rnd 3) 1))))
    ;; Bignums of up to 400 bits, and ones with long runs of zero or
    ;; one bits.
    ((7) (sign (rnd (expt 2 (+ 62 (rnd 340))))))
    (else (sign (- (expt 2 (+ 30 (rnd 200))) (expt 2 (rnd 30)))))))

(define (nonzero-int)
  (let ((x (rand-int))) (if (zero? x) (+ 1 (rnd 7)) x)))

(define (rand-rat)
  (if (zero? (rnd 3)) (rand-int) (/ (rand-int) (nonzero-int))))

(define (nonzero-rat)
  (let ((x (rand-rat))) (if (zero? x) 1/3 x)))

;; A small rational, for EXPT's base.
(define (small-rat)
  (let ((n (sign (rnd 50))) (d (+ 1 (rnd 30))))
    (/ n d)))

(define (lognand a b) (lognot (logand a b)))
(define (lognor a b) (lognot (logior a b)))
(define (logequiv a b) (lognot (logxor a b)))
(define (logandca a b) (logand a (lognot b)))
(define (isqrt n) (call-with-values (lambda () (exact-integer-sqrt n))
		    (lambda (s r) s)))

;; (NAME GENERATOR FUNCTION): GENERATOR makes an argument list,
;; FUNCTION the answer.
(define ops
  (list
   (list '+ (lambda () (list (rand-rat) (rand-rat))) +)
   (list '- (lambda () (list (rand-rat) (rand-rat))) -)
   (list '* (lambda () (list (rand-rat) (rand-rat))) *)
   (list '/ (lambda () (list (rand-rat) (nonzero-rat))) /)
   (list 'negate (lambda () (list (rand-rat))) -)
   (list '+int (lambda () (list (rand-int) (rand-int))) +)
   (list '-int (lambda () (list (rand-int) (rand-int))) -)
   (list '*int (lambda () (list (rand-int) (rand-int))) *)
   (list '/int (lambda () (list (rand-int) (nonzero-int))) /)
   (list 'quotient (lambda () (list (rand-int) (nonzero-int))) quotient)
   (list 'remainder (lambda () (list (rand-int) (nonzero-int))) remainder)
   (list 'modulo (lambda () (list (rand-int) (nonzero-int))) modulo)
   (list 'gcd (lambda () (list (rand-int) (rand-int))) gcd)
   (list 'lcm (lambda () (list (rand-int) (rand-int))) lcm)
   (list 'abs (lambda () (list (rand-rat))) abs)
   (list 'max (lambda () (list (rand-rat) (rand-rat))) max)
   (list 'min (lambda () (list (rand-rat) (rand-rat))) min)
   (list 'numerator (lambda () (list (rand-rat))) numerator)
   (list 'denominator (lambda () (list (rand-rat))) denominator)
   (list 'floor (lambda () (list (rand-rat))) floor)
   (list 'ceiling (lambda () (list (rand-rat))) ceiling)
   (list 'truncate (lambda () (list (rand-rat))) truncate)
   (list 'round (lambda () (list (rand-rat))) round)
   (list 'expt
	 (lambda ()
	   (if (zero? (rnd 2))
	       (let ((b (small-rat)))
		 (list (if (zero? b) 2 b) (- (rnd 60) 20)))
	       (list (rand-int) (rnd 4))))
	 expt)
   (list 'sqrt (lambda () (let ((r (rand-rat))) (list (* r r))))
	 (lambda (x) (sqrt x)))
   (list 'integer-sqrt (lambda () (list (abs (rand-int)))) isqrt)
   (list '= (lambda () (let ((a (rand-rat))) (list a (if (zero? (rnd 3)) a (rand-rat))))) =)
   (list '< (lambda () (list (rand-rat) (rand-rat))) <)
   (list '> (lambda () (list (rand-rat) (rand-rat))) >)
   (list '<= (lambda () (let ((a (rand-rat))) (list a (if (zero? (rnd 3)) a (rand-rat))))) <=)
   (list '>= (lambda () (let ((a (rand-rat))) (list a (if (zero? (rnd 3)) a (rand-rat))))) >=)
   (list 'zero? (lambda () (list (rand-rat))) zero?)
   (list 'positive? (lambda () (list (rand-rat))) positive?)
   (list 'negative? (lambda () (list (rand-rat))) negative?)
   (list 'even? (lambda () (list (rand-int))) even?)
   (list 'odd? (lambda () (list (rand-int))) odd?)
   (list 'bit-not (lambda () (list (rand-int))) lognot)
   (list 'bit-and (lambda () (list (rand-int) (rand-int))) logand)
   (list 'bit-or (lambda () (list (rand-int) (rand-int))) logior)
   (list 'bit-xor (lambda () (list (rand-int) (rand-int))) logxor)
   (list 'bit-nand (lambda () (list (rand-int) (rand-int))) lognand)
   (list 'bit-nor (lambda () (list (rand-int) (rand-int))) lognor)
   (list 'bit-equiv (lambda () (list (rand-int) (rand-int))) logequiv)
   (list 'bit-andca (lambda () (list (rand-int) (rand-int))) logandca)
   (list 'ash-left (lambda () (list (rand-int) (- (rnd 300) 100))) ash)
   (list 'ash-right (lambda () (list (rand-int) (- (rnd 300) 200)))
	 (lambda (x n) (ash x (- n))))
   ;; The printer in a radix, and the reader back: the answer of
   ;; PRINT-RADIX is upper case, as Oaklisp prints; READ-RADIX gets
   ;; lower or upper case digits.
   (list 'print-radix (lambda () (list (rand-rat) (+ 2 (rnd 35))))
	 (lambda (x r) (string-upcase (number->string x r))))
   (list 'read-radix
	 (lambda ()
	   (let* ((x (rand-rat)) (r (+ 2 (rnd 35))) (s (number->string x r)))
	     (list (if (zero? (rnd 2)) s (string-upcase s)) r)))
	 (lambda (s r) (string->number s r)))
   (list 'read-prefixed
	 (lambda ()
	   (let* ((x (rand-rat))
		  (p (pick '(("#b" . 2) ("#o" . 8) ("#d" . 10) ("#x" . 16)))))
	     (list (string-append (car p) (number->string x (cdr p))))))
	 (lambda (s) (string->number s)))
   ;; Junk over the characters numbers are made of: what Guile
   ;; parses exactly, Oaklisp must parse to the same number, and what
   ;; Guile refuses, Oaklisp must too (an inexact parse is skipped).
   (list 'parse-junk
	 (lambda ()
	   (let retry ()
	     (let* ((r (pick '(2 8 10 16)))
		    (s (list->string
			(map (lambda (i) (string-ref "0123456789abcdefABCDEF+-/#xXbBoOdD"
						     (rnd 34)))
			     (iota (+ 1 (rnd 7))))))
		    (v (false-if-exception (string->number s r))))
	       (if (and v (inexact? v)) (retry) (list s r)))))
	 (lambda (s r) (false-if-exception (string->number s r))))
   (list 'read-datum
	 (lambda ()
	   (let* ((x (rand-rat))
		  (p (pick '(("" . 10) ("#b" . 2) ("#o" . 8) ("#x" . 16)))))
	     (list (string-append (car p) (number->string x (cdr p))))))
	 (lambda (s) (string->number s)))))

(with-output-to-file outfile
  (lambda ()
    (format #t ";;; ~a cases per operation, seed ~a; generated by numeric-cases.scm~%"
	    count seed)
    (for-each
     (lambda (op)
       (let ((name (car op)) (gen (cadr op)) (f (caddr op)))
	 (do ((i 0 (+ i 1))) ((= i count))
	   (let ((a (gen)))
	     (write (list name a (apply f a)))
	     (newline)))))
     ops)))
