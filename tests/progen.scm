;;; progen.scm -- random Oaklisp programs for differential testing
;;; (fuzz.sh runs each in every mode and compares the output).
;;;
;;;   guile progen.scm SEED OUTFILE
;;;
;;; The program is typed as it is made: every expression is built to
;;; give a value of a known type (integer, rational, boolean, list of
;;; integers, vector of integers, string, function from integer to
;;; integer, object of a generated type), so no operation meets a value
;;; it cannot take, divisors are kept from zero, and loops and
;;; recursions run a bounded number of times.  So the program runs to
;;; the end, and what it prints -- one "=> TAG VALUE" line per result --
;;; depends on nothing but the language: no hash numbers, no addresses,
;;; no timing.  The errors it does provoke are caught by the program
;;; itself (CATCH-ERRORS), so traps are exercised too.

(define args (cdr (command-line)))
(define seed (string->number (list-ref args 0)))
(define outfile (list-ref args 1))
(define st (seed->random-state seed))

(define (rnd n) (random n st))
(define (pick l) (list-ref l (rnd (length l))))
(define (chance n) (zero? (rnd n)))	; one chance in N

(define counter 0)
(define (fresh prefix)
  (set! counter (+ counter 1))
  (string->symbol (string-append prefix (number->string counter))))

;;; Leaves.

(define (int-literal)
  (case (rnd 7)
    ((0 1 2) (- (rnd 41) 20))
    ((3) (- (rnd 2000001) 1000000))
    ((4) (pick (list (expt 2 29) (- (expt 2 29)) (- (expt 2 29) 1)
		     (expt 2 61) (- (expt 2 61)) (- (expt 2 61) 1)
		     (+ (expt 2 61) 1) (- -1 (expt 2 61)))))
    ((5) (let ((x (random (expt 2 (+ 62 (rnd 70))) st))) (if (chance 2) x (- x))))
    (else (- (* (expt 10 (* 9 (+ 1 (rnd 3)))) (if (chance 2) 1 -1)) (- (rnd 3) 1)))))

(define string-chars "abcxyzABC019 -+*/.\"\\")
(define (string-literal)
  (list->string
   (map (lambda (i) (string-ref string-chars (rnd (string-length string-chars))))
	(iota (rnd 6)))))

;;; The environment: local variables with their types, an alist.

(define (vars-of env type)
  (map car (filter (lambda (b) (eq? (cdr b) type)) env)))

;;; Global functions made so far: (NAME ARG-TYPES RESULT-TYPE).  A body
;;; calls only functions made before it, so no recursion but the
;;; bounded kind each function makes for itself.
(define globals '())

(define (callable type)
  (filter (lambda (g) (eq? (caddr g) type)) globals))

;;; An expression of TYPE in ENV, at most DEPTH deep.

(define (gen type env depth)
  (if (or (<= depth 0) (chance (+ 2 (* 2 depth))))
      (leaf type env depth)
      (case type
	((int) (gen-int env depth))
	((rat) (gen-rat env depth))
	((bool) (gen-bool env depth))
	((list) (gen-list env depth))
	((vec) (gen-vec env depth))
	((str) (gen-str env depth))
	((fn) (gen-fn env depth))
	((obj) (gen-obj env depth)))))

(define (leaf type env depth)
  (let ((vs (vars-of env type)))
    (if (and (pair? vs) (not (chance 3)))
	(pick vs)
	(case type
	  ((int) (if (chance 6) '(fluid zfl) (int-literal)))
	  ((rat) (if (chance 2) (int-literal) `(/ ,(int-literal) ,(+ 1 (rnd 30)))))
	  ((bool) (pick '(#t #f)))
	  ((list) (if (chance 3) ''() `(list ,@(map (lambda (i) (int-literal)) (iota (rnd 4))))))
	  ((vec) `(vector ,@(map (lambda (i) (int-literal)) (iota (+ 1 (rnd 3))))))
	  ((str) (string-literal))
	  ((fn) (let ((x (fresh "x")))
		  `(lambda (,x) ,(body-gen 'int (cons (cons x 'int) env) 1))))
	  ((obj) `(make zt ,(int-literal) ,(int-literal)))))))

(define (g type env depth) (gen type env (- depth 1)))

;;; Inside a body that may run many times -- a loop's, a recursion's, a
;;; function's -- the heavy expressions (deep recursion, the allocation
;;; loop) are left out, so that their cost does not multiply.
(define in-loop 0)
(define (body thunk)
  (set! in-loop (+ in-loop 1))
  (let ((r (thunk)))
    (set! in-loop (- in-loop 1))
    r))
(define (body-gen type env depth) (body (lambda () (gen type env depth))))

;;; A binding of a new local of TYPE around BODY-MAKER's expression.
(define (with-local type env depth body-maker)
  (let ((v (fresh "v")))
    `(let ((,v ,(g type env depth)))
       ,(body-maker v (cons (cons v type) env)))))

(define (gen-int env depth)
  (case (let choose () (let ((c (rnd 55)))
			 (if (and (> in-loop 0) (memv c '(39 40 41 42))) (choose) c)))
    ((0 1 2) `(+ ,(g 'int env depth) ,(g 'int env depth)))
    ((3 4) `(- ,(g 'int env depth) ,(g 'int env depth)))
    ((5) `(* ,(g 'int env depth) ,(g 'int env depth)))
    ((6) `(,(pick '(quotient remainder modulo)) ,(g 'int env depth) (znz ,(g 'int env depth))))
    ((7) `(,(pick '(abs - 1+)) ,(g 'int env depth)))
    ((8) `(,(pick '(max min)) ,(g 'int env depth) ,(g 'int env depth)))
    ((9) `(if ,(g 'bool env depth) ,(g 'int env depth) ,(g 'int env depth)))
    ((10) `(cond (,(g 'bool env depth) ,(g 'int env depth))
		 (,(g 'bool env depth) ,(g 'int env depth))
		 (else ,(g 'int env depth))))
    ((11) `(case (modulo ,(g 'int env depth) 4)
	     ((0) ,(g 'int env depth)) ((1 2) ,(g 'int env depth)) (else ,(g 'int env depth))))
    ((12) (with-local (pick '(int list fn obj)) env depth
		      (lambda (v env2) (gen 'int env2 (- depth 1)))))
    ((13) `(length ,(g (pick '(list vec str)) env depth)))
    ((14) (with-local 'list env depth
		      (lambda (v env2) `(if (null? ,v) ,(g 'int env2 depth) (car ,v)))))
    ((15) (with-local 'vec env depth
		      (lambda (v env2) `(nth ,v (modulo ,(g 'int env2 depth) (length ,v))))))
    ((16) `(,(g 'fn env depth) ,(g 'int env depth)))
    ((17) (let ((fs (callable 'int)))
	    (if (null? fs) (gen-int env depth) (call-global (pick fs) env depth))))
    ((18) `(zsum ,(g 'obj env depth) ,(g 'int env depth)))
    ((19) `(,(pick '(za zb)) ,(g 'obj env depth)))
    ((20) `(zkind ,(g (pick '(int rat list vec str obj fn)) env depth)))
    ((21) (let ((lp (fresh "zl")) (i (fresh "i")) (acc (fresh "acc")))
	    `(let ,lp ((,i 0) (,acc ,(g 'int env depth)))
	       (if (>= ,i ,(rnd 7)) ,acc
		   (,lp (+ ,i 1) ,(body-gen 'int (cons* (cons i 'int) (cons acc 'int) env) (- depth 1)))))))
    ((22) (let ((acc (fresh "acc")) (i (fresh "i")))
	    `(let ((,acc ,(g 'int env depth)))
	       (dotimes (,i ,(rnd 6))
		 (set! ,acc ,(body-gen 'int (cons* (cons i 'int) (cons acc 'int) env) (- depth 1))))
	       ,acc)))
    ((23) (let ((f (fresh "zr")) (n (fresh "n")))
	    `(labels (((,f ,n) (if (<= ,n 0) ,(g 'int env depth)
				   (+ ,(body-gen 'int (cons (cons n 'int) env) (- depth 1))
				      (,f (- ,n 1))))))
	       (,f ,(rnd 8)))))
    ((24) (let ((t (fresh "t")))
	    `(native-catch ,t (+ ,(g 'int env depth)
				 (if ,(g 'bool env depth) (throw ,t ,(g 'int env depth))
				     ,(g 'int env depth))))))
    ((25) (let ((k (fresh "k")))
	    `(catch ,k (+ ,(g 'int env depth)
			  (if ,(g 'bool env depth) (,k ,(g 'int env depth)) ,(g 'int env depth))))))
    ((26) (let ((k (fresh "k")))
	    `(call/cc (lambda (,k) (+ ,(g 'int env depth)
				      (if ,(g 'bool env depth) (,k ,(g 'int env depth))
					  ,(g 'int env depth)))))))
    ((27) `(catch-errors (general-error (lambda (e) ,(g 'int env depth)))
	     ,(pick (list `(quotient ,(g 'int env depth) (- (zid 3) 3))
			  `(car (zid ,(g 'int env depth)))
			  `(+ 1 (zid ,(g 'list env depth)))
			  `(error "Boom ~D." ,(g 'int env depth))
			  `(nth (vector 1 2) (+ 2 (zid ,(rnd 3))))))))
    ((28) (with-local 'int env depth
		      (lambda (v env2) `(block (set! ,v ,(gen 'int env2 (- depth 1))) ,v))))
    ((29) (let ((v (fresh "v")) (f (fresh "f")))
	    `(let* ((,v ,(g 'int env depth)) (,f (lambda () ,v)))
	       (set! ,v ,(g 'int env depth))
	       (,f))))
    ((30) (let ((gv (pick '(zg1 zg2))))
	    `(block (set! ,gv (+ ,gv ,(g 'int env depth))) ,gv)))
    ((31) `(bind (((fluid zfl) ,(g 'int env depth))) ,(g 'int env depth)))
    ((32) `(wind-protect (set! zg1 (+ zg1 1)) ,(g 'int env depth) (set! zg2 (+ zg2 1))))
    ((33) `(apply ,(pick '(+ max min)) ,(g 'int env depth) ,(g 'list env depth)))
    ((34) `(zrest ,@(map (lambda (i) (g 'int env depth)) (iota (+ 1 (rnd 4))))))
    ((35) `(zpass ,@(map (lambda (i) (g 'int env depth)) (iota (+ 1 (rnd 4))))))
    ((36) `(,(pick '(bit-and bit-or bit-xor)) ,(g 'int env depth) ,(g 'int env depth)))
    ((37) `(,(pick '(ash-left ash-right)) ,(g 'int env depth) ,(rnd 70)))
    ((38) `(,(pick '(floor ceiling round truncate numerator denominator)) ,(g 'rat env depth)))
    ;; Recursion deep enough to spill the stacks into heap segments, and
    ;; escapes from the bottom of it.
    ((39) `(zdeep ,(+ 500 (rnd 4000)) ,(g 'int env depth)))
    ((40) (let ((k (fresh "k")))
	    `(call/cc (lambda (,k) (zdive ,(+ 300 (rnd 3000)) ,k ,(g 'int env depth))))))
    ((41) (let ((t (fresh "t")) (v (fresh "v")))
	    `(native-catch ,t (zdive ,(+ 300 (rnd 3000)) (lambda (,v) (throw ,t ,v))
				     ,(g 'int env depth)))))
    ;; Garbage, so that collections come in the middle of things.
    ((42) `(zchurn ,(rnd 60000) ,(g 'int env depth)))
    ((43) (let ((f (fresh "zi")) (x (fresh "x")))
	    `(let () (define (,f ,x) ,(body-gen 'int (cons (cons x 'int) env) (- depth 1)))
		  (,f ,(g 'int env depth)))))
    ((44) (let ((i (fresh "i")) (acc (fresh "acc")))
	    `(do ((,i 0 (+ ,i 1))
		  (,acc ,(g 'int env depth)
			,(body-gen 'int (cons* (cons i 'int) (cons acc 'int) env) (- depth 1))))
		 ((>= ,i ,(rnd 6)) ,acc))))
    ((45) (with-local 'int env depth
		      (lambda (v env2)
			`(block (when ,(g 'bool env2 depth) (set! ,v ,(g 'int env2 depth)))
				(unless ,(g 'bool env2 depth) (set! ,v ,(g 'int env2 depth)))
				,v))))
    ((46) (let ((s (fresh "s")))
	    `(let ((,s (append "q" ,(g 'str env depth))))
	       ((coercer number) (nth ,s (modulo ,(g 'int env depth) (length ,s)))))))
    ((47) `(case (nth '(a b c d) (modulo ,(g 'int env depth) 4))
	     ((a b) ,(g 'int env depth)) ((c) ,(g 'int env depth)) (else ,(g 'int env depth))))
    ((48) (let ((h (fresh "h")) (k1 (rnd 5)) (k2 (rnd 5)))
	    `(let ((,h (make-eq-hash-table)))
	       (set! (table-entry ,h ,k1) ,(g 'int env depth))
	       (set! (table-entry ,h ,(rnd 5)) ,(g 'int env depth))
	       (or (table-entry ,h ,k2) ,(g 'int env depth)))))
    ((49) (let ((h (fresh "h")) (l (fresh "l")))
	    `(let ((,h (make-equal-hash-table)) (,l ,(g 'list env depth)))
	       (set! (table-entry ,h ,l) ,(g 'int env depth))
	       (set! (table-entry ,h ,(g 'str env depth)) ,(g 'int env depth))
	       (or (table-entry ,h (append ,l '())) ,(g 'int env depth)))))
    ((50) (let ((r (+ 2 (rnd 35))) (v (fresh "v")))
	    ;; The number is made outside the radix's binding, which
	    ;; would print anything printed within it in that radix.
	    `(let ((,v ,(g 'int env depth)))
	       (string->number (bind (((fluid print-radix) ,r)) (format #f "~A" ,v)) ,r))))
    ((51) `(numerator (string->number (format #f "~A" ,(g 'rat env depth)))))
    ((52) `(apply zsum (list ,(g 'obj env depth) ,(g 'int env depth))))
    ((54) (let ((op (fresh "op")) (name (pick '(zsum zrest max +))))
	    `(let ((,op ,name))
	       (,op ,(g (if (eq? name 'zsum) 'obj 'int) env depth) ,(g 'int env depth)))))
    ((53) (with-local 'obj env depth
		      (lambda (v env2)
			`(block (zset-a ,v ,(g 'int env2 depth)) (zsum ,v ,(g 'int env2 depth))))))
    (else (with-local 'vec env depth
		      (lambda (v env2)
			`(block (set! (nth ,v (modulo ,(g 'int env2 depth) (length ,v)))
				      ,(g 'int env2 depth))
				(apply + ((coercer list-type) ,v))))))))

(define (gen-rat env depth)
  (case (rnd 6)
    ((0) `(/ ,(g 'int env depth) (znz ,(g 'int env depth))))
    ((1) `(+ ,(g 'rat env depth) ,(g 'rat env depth)))
    ((2) `(* ,(g 'rat env depth) ,(g 'rat env depth)))
    ((3) `(- ,(g 'rat env depth) ,(g 'rat env depth)))
    ((4) `(/ ,(g 'rat env depth) (znz ,(g 'rat env depth))))
    (else `(if ,(g 'bool env depth) ,(g 'rat env depth) ,(g 'int env depth)))))

(define (gen-bool env depth)
  (case (rnd 12)
    ((0 1) `(,(pick '(< <= > >= =)) ,(g 'int env depth) ,(g 'int env depth)))
    ((2) `(,(pick '(< =)) ,(g 'rat env depth) ,(g 'rat env depth)))
    ((3) `(,(pick '(zero? even? odd? negative?)) ,(g 'int env depth)))
    ((4) `(,(pick '(null? pair?)) ,(g 'list env depth)))
    ((5) `(equal? ,(g 'list env depth) ,(g 'list env depth)))
    ((6) `(not ,(g 'bool env depth)))
    ((7) `(and ,(g 'bool env depth) ,(g 'bool env depth)))
    ((8) `(or ,(g 'bool env depth) ,(g 'bool env depth)))
    ((9) `(if (memq ,(g 'int env depth) ,(g 'list env depth)) #t #f))
    ((10) (pick (list `(< ,(g 'int env depth) ,(g 'int env depth) ,(g 'int env depth))
		      `(equal? ,(g 'vec env depth) ,(g 'vec env depth))
		      `(equal? ,(g 'str env depth) ,(g 'str env depth)))))
    (else (let ((fs (callable 'bool)))
	    (if (null? fs) `(zero? ,(g 'int env depth)) (call-global (pick fs) env depth))))))

(define (gen-list env depth)
  (case (rnd 14)
    ((0) `(cons ,(g 'int env depth) ,(g 'list env depth)))
    ((1) (with-local 'list env depth (lambda (v env2) `(if (null? ,v) ,v (cdr ,v)))))
    ((2) `(append ,(g 'list env depth) ,(g 'list env depth)))
    ((3) `(reverse ,(g 'list env depth)))
    ((4) `(map ,(g 'fn env depth) ,(g 'list env depth)))
    ((5) (let ((x (fresh "x")))
	   `(map (lambda (,x) ,(body-gen 'int (cons (cons x 'int) env) (- depth 1)))
		 ,(g 'list env depth))))
    ((6) `((coercer list-type) ,(g 'vec env depth)))
    ((7) `(sort ,(g 'list env depth) ,(pick '(< >))))
    ((8) `(iota ,(rnd 6)))
    ((9) `(list ,@(map (lambda (i) (g 'int env depth)) (iota (rnd 4)))))
    ((10) (let ((acc (fresh "acc")) (x (fresh "x")))
	    `(let ((,acc '()))
	       (for-each (lambda (,x) (set! ,acc (cons (* 2 ,x) ,acc))) ,(g 'list env depth))
	       ,acc)))
    ;; Printed and read back in.
    ((12) `(read (make string-input-stream (format #f "~S" ,(g 'list env depth)))))
    ;; An operation as a value.
    ((13) `(map ,(pick '(za zb)) (list ,(g 'obj env depth) ,(g 'obj env depth))))
    (else (let ((fs (callable 'list)))
	    (if (null? fs) `(list ,(g 'int env depth)) (call-global (pick fs) env depth))))))

(define (gen-vec env depth)
  (case (rnd 3)
    ((0) `(vector ,@(map (lambda (i) (g 'int env depth)) (iota (+ 1 (rnd 4))))))
    ((1) `((coercer simple-vector) (cons ,(g 'int env depth) ,(g 'list env depth))))
    (else (with-local 'vec env depth
		      (lambda (v env2) `(block (set! (nth ,v 0) ,(g 'int env2 depth)) ,v))))))

(define (gen-str env depth)
  (case (rnd 4)
    ((0) `(append ,(g 'str env depth) ,(g 'str env depth)))
    ((1) `(upcase ,(g 'str env depth)))
    ((2) `(format #f "~A/~S" ,(g 'int env depth) ,(g (pick '(rat list str)) env depth)))
    (else (string-literal))))

(define (gen-fn env depth)
  (case (rnd 4)
    ((0) (let ((x (fresh "x")))
	   `(lambda (,x) ,(body-gen 'int (cons (cons x 'int) env) (- depth 1)))))
    ((1) (let ((k (fresh "k")) (x (fresh "x")))
	   `(let ((,k ,(g 'int env depth))) (lambda (,x) (+ ,x ,k)))))
    ((2) (let ((fs (filter (lambda (g) (equal? (cadr g) '(int))) (callable 'int))))
	   (if (null? fs) (gen-fn env depth) (car (pick fs)))))
    (else (let ((f (fresh "f")) (x (fresh "x")))
	    `(let ((,f ,(g 'fn env depth))) (lambda (,x) (,f (,f ,x))))))))

(define (gen-obj env depth)
  (case (rnd 3)
    ((0) `(make zt ,(g 'int env depth) ,(g 'int env depth)))
    ((1) `(make zt2 ,(g 'int env depth) ,(g 'int env depth) ,(g 'int env depth)))
    (else (with-local 'obj env depth
		      (lambda (v env2) `(block (zset-a ,v ,(g 'int env2 depth)) ,v))))))

;;; A call of global G with arguments made for its parameter types.  A
;;; global with a counter (its first parameter "n") gets a small one.
(define (call-global g env depth)
  (let ((name (car g)) (types (cadr g)))
    `(,name ,@(map (lambda (t) (if (eq? t 'count) (rnd 8) (g* t env depth))) types))))
(define (g* type env depth) (gen type env (- depth 1)))

;;; A global function: random parameters and result, sometimes a
;;; bounded recursion on a counter.
(define (make-pair-of-globals) (body make-pair-of-globals-1))
(define (make-pair-of-globals-1)
  ;; Two functions on a counter, each calling the other.
  (let* ((a (fresh "zf")) (b (fresh "zf")) (n (fresh "n")) (p (fresh "p"))
	 (env (list (cons p 'int))))
    ;; The pair is callable only once both bodies are made: a body
    ;; calling its partner with a fresh counter would never end.
    (let ((forms
	   (list `(define (,a ,n ,p) (if (<= ,n 0) ,(gen 'int env 3)
					 (+ ,(gen 'int (cons (cons n 'int) env) 2) (,b (- ,n 1) ,p))))
		 `(define (,b ,n ,p) (if (<= ,n 0) ,(gen 'int env 3)
					 (- ,(gen 'int (cons (cons n 'int) env) 2) (,a (- ,n 1) ,p)))))))
      (set! globals (cons* (list a '(count int) 'int) (list b '(count int) 'int) globals))
      forms)))

(define (make-global) (body make-global-1))
(define (make-global-1)
  (let* ((name (fresh "zf"))
	 (result (pick '(int int int bool list)))
	 (ptypes (map (lambda (i) (pick '(int int list fn obj rat))) (iota (rnd 4))))
	 (params (map (lambda (t) (fresh "p")) ptypes))
	 (env (map cons params ptypes))
	 (recursive (chance 3))
	 (form
	  (if recursive
	      (let ((n (fresh "n")))
		`(define (,name ,n ,@params)
		   (if (<= ,n 0)
		       ,(gen result env 3)
		       ,(case result
			  ((int) `(+ ,(gen 'int (cons (cons n 'int) env) 2)
				     (,name (- ,n 1) ,@params)))
			  ((bool) `(not (,name (- ,n 1) ,@params)))
			  (else `(cons ,n (,name (- ,n 1) ,@params)))))))
	      `(define (,name ,@params) ,(gen result env 4)))))
    (set! globals (cons (list name (if recursive (cons 'count ptypes) ptypes) result) globals))
    form))

(define prelude
  '((set! (fluid print-length) #f)
    (set! (fluid print-level) #f)
    ;; The marker "=> " is made as the line is printed, so that the
    ;; loader's echo of this form, loading source, never shows it.
    (define (show tag x) (format #t "~C> ~A ~S~%" #\= tag x))
    (define (znz x) (if (zero? x) 7 x))
    (define (zid x) x)
    (define zg1 0)
    (define zg2 0)
    (set! (fluid zfl) 3)
    (define-instance zt type '(a b) (list object))
    (add-method (initialize (zt a b) self x y) (set! a x) (set! b y) self)
    (define-instance za operation)
    (add-method (za (zt a) self) a)
    (define-instance zb operation)
    (add-method (zb (zt b) self) b)
    (define-instance zset-a operation)
    (add-method (zset-a (zt a) self x) (set! a x))
    (define-instance zsum operation)
    (add-method (zsum (zt a b) self k) (+ a (* 2 b) k))
    (define-instance zt2 type '(c) (list zt))
    (add-method (initialize (zt2 c) self x y z) (set! c z) (^super zt initialize self x y))
    (add-method (zsum (zt2 c) self k) (- (^super zt zsum self k) c))
    (define-instance zkind operation)
    (add-method (zkind (object) self) 0)
    (add-method (zkind (fixnum) self) 1)
    (add-method (zkind (bignum) self) 2)
    (add-method (zkind (fraction) self) 3)
    (add-method (zkind (cons-pair) self) (+ 10 (length self)))
    (add-method (zkind (string) self) (+ 20 (length self)))
    (add-method (zkind (simple-vector) self) (+ 30 (length self)))
    (add-method (zkind (zt a) self) (+ 40 (zkind a)))
    (add-method (zkind (zt2 c) self) (+ 50 (^super zt zkind self)))
    (define (zrest a . r) (+ a (apply + r)))
    (define (zdeep n acc) (if (zero? n) acc (+ 1 (zdeep (- n 1) acc))))
    (define (zdive n k v) (if (zero? n) (k v) (+ 1 (zdive (- n 1) k v))))
    (define (zchurn n x)
      (let loop ((i 0) (acc x) (keep '()))
	(if (= i n) (+ acc (length keep))
	    (loop (+ i 1) (+ acc (bit-and (length (list i i i)) 1))
		  (if (zero? (remainder i 1000)) (cons (vector i) keep) keep)))))
    (define (zpass a . r) (zrest (* 2 a) . r))))

(define (program)
  (let* ((gforms (apply append
		       (map (lambda (i) (if (chance 4) (make-pair-of-globals) (list (make-global))))
			    (iota (+ 2 (rnd 10))))))
	 (tag 0)
	 (show (lambda ()
		 (set! tag (+ tag 1))
		 `(show ,tag ,(gen (pick '(int int int rat bool list vec str obj-int)) '() 5))))
	 (top (map (lambda (i) (show)) (iota (+ 8 (rnd 16)))))
	 (main (map (lambda (i) (show)) (iota (+ 3 (rnd 6))))))
    (append prelude gforms top
	    `((define (zmain) ,@main) (zmain)
	      (show "globals" (list zg1 zg2 (fluid zfl)))))))

;;; OBJ-INT shows an object through what it holds.
(define old-gen gen)
(set! gen (lambda (type env depth)
	    (if (eq? type 'obj-int)
		`(let ((o ,(old-gen 'obj env depth))) (list (za o) (zb o) (zsum o 1)))
		(old-gen type env depth))))

;;; Written as Oaklisp reads it: Guile's WRITE would put a symbol such
;;; as 1+ in #{ }# brackets.
(define (oak-write x)
  (cond ((pair? x)
	 (display "(")
	 (let loop ((x x) (first #t))
	   (cond ((null? x))
		 ((pair? x)
		  (unless first (display " "))
		  (oak-write (car x))
		  (loop (cdr x) #f))
		 (else (display " . ") (oak-write x))))
	 (display ")"))
	((null? x) (display "()"))
	((symbol? x) (display (symbol->string x)))
	(else (write x))))

(with-output-to-file outfile
  (lambda ()
    (format #t ";;; Generated by progen.scm, seed ~a.~%" seed)
    (for-each (lambda (form) (oak-write form) (newline)) (program))))
