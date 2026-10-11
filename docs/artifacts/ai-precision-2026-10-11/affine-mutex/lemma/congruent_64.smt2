; 
(set-info :status unknown)
(declare-fun b () (_ BitVec 64))
(declare-fun a () (_ BitVec 64))
(declare-fun byte_b () (_ BitVec 64))
(declare-fun byte_a () (_ BitVec 64))
(assert
 (let ((?x55 (bvadd b (_ bv0 64))))
 (let ((?x7 (bvurem ?x55 (_ bv8 64))))
 (let ((?x53 (bvurem a (_ bv8 64))))
 (= ?x53 ?x7)))))
(assert
 (let (($x30 (bvult byte_b (_ bv8 64))))
 (let (($x31 (bvult byte_a (_ bv4 64))))
 (and $x31 $x30))))
(assert
 (let ((?x22 (bvadd b byte_b)))
 (let ((?x21 (bvadd a byte_a)))
 (= ?x21 ?x22))))
(assert
 (and (distinct a b) true))
(check-sat)
