Examples
========

All examples are in the `examples/ <https://github.com/hnmr293/aleff/tree/main/examples>`_ directory.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Example
     - Description
   * - `N-Queens <https://github.com/hnmr293/aleff/blob/main/examples/demo_nqueens.py>`_
     - Backtracking search via multi-shot continuations
   * - `Amb / Logic puzzle <https://github.com/hnmr293/aleff/blob/main/examples/demo_amb.py>`_
     - Scheme-style ``amb`` operator and constraint solving (SICP Exercise 4.42)
   * - `Probability <https://github.com/hnmr293/aleff/blob/main/examples/demo_probability.py>`_
     - Exact discrete probability distributions via weighted multi-shot
   * - `Dependency injection <https://github.com/hnmr293/aleff/blob/main/examples/demo_di.py>`_
     - Swap DB/email/logging implementations
   * - `Record/Replay <https://github.com/hnmr293/aleff/blob/main/examples/demo_record_replay.py>`_
     - Record effect results, replay without side effects
   * - `Transactions <https://github.com/hnmr293/aleff/blob/main/examples/demo_transaction.py>`_
     - Buffer writes, commit on success, rollback on failure
   * - `Automatic differentiation <https://github.com/hnmr293/aleff/blob/main/examples/demo_autodiff.py>`_
     - Forward-mode (dual numbers) and reverse-mode (backpropagation)
   * - `Shallow state machine <https://github.com/hnmr293/aleff/blob/main/examples/demo_shallow_state.py>`_
     - Mutable state (get/put) and traffic light controller
   * - `shift/reset, shift0/reset0 <https://github.com/hnmr293/aleff/blob/main/examples/demo_shift_reset.py>`_
     - Delimited continuations: deep = shift/reset, shallow = shift0/reset0
