"""
Demo: Transaction (commit / rollback)

Implement transaction semantics via effect handlers.
- Business logic is written without awareness of side effects
- Handlers buffer operations and commit on success or rollback on failure
- Abort (no resume) allows the handler to cancel the entire transaction
"""

from dataclasses import dataclass, field

from aleff import (
    effect,
    Effect,
    Resume,
    create_handler,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


@dataclass
class Account:
    name: str
    balance: int


# ---------------------------------------------------------------------------
# Effects
# ---------------------------------------------------------------------------

get_balance: Effect[[str], int] = effect("get_balance")
transfer: Effect[[str, str, int], None] = effect("transfer")  # from, to, amount
log: Effect[[str], None] = effect("log")


# ---------------------------------------------------------------------------
# Business logic
# ---------------------------------------------------------------------------


def process_payroll(company: str, employees: list[tuple[str, int]]) -> str:
    """Transfer salary from company account to each employee."""
    company_balance = get_balance(company)
    total = sum(amount for _, amount in employees)

    log(f"payroll: {company} -> {len(employees)} employees, total={total}")
    log(f"company balance: {company_balance}")

    if company_balance < total:
        log(f"insufficient funds: {company_balance} < {total}")
        return "error: insufficient funds"

    for name, amount in employees:
        log(f"  transfer {amount} to {name}")
        transfer(company, name, amount)

    remaining = get_balance(company)
    log(f"payroll complete. remaining balance: {remaining}")

    return "ok"


# ---------------------------------------------------------------------------
# In-memory database
# ---------------------------------------------------------------------------


@dataclass
class Database:
    accounts: dict[str, Account] = field(default_factory=dict[str, Account])

    def snapshot(self) -> dict[str, int]:
        return {name: acc.balance for name, acc in sorted(self.accounts.items())}


def create_test_db() -> Database:
    db = Database()
    db.accounts["Acme Corp"] = Account("Acme Corp", 10000)
    db.accounts["Alice"] = Account("Alice", 500)
    db.accounts["Bob"] = Account("Bob", 300)
    db.accounts["Charlie"] = Account("Charlie", 200)
    return db


# ---------------------------------------------------------------------------
# Handler 1: Auto-commit (each operation is immediately applied)
# ---------------------------------------------------------------------------


def run_autocommit(db: Database, employees: list[tuple[str, int]]) -> str:
    print("=== Auto-commit ===")

    h = create_handler(get_balance, transfer, log)

    @h.on(get_balance)
    def _get(k: Resume[int, str], name: str):
        return k(db.accounts[name].balance)

    @h.on(transfer)
    def _transfer(k: Resume[None, str], src: str, dst: str, amount: int):
        db.accounts[src].balance -= amount
        db.accounts[dst].balance += amount
        return k(None)

    @h.on(log)
    def _log(k: Resume[None, str], msg: str):
        print(f"  [LOG] {msg}")
        return k(None)

    result = h(lambda: process_payroll("Acme Corp", employees))
    print(f"  result: {result}")
    print(f"  db: {db.snapshot()}")
    return result


# ---------------------------------------------------------------------------
# Handler 2: Transactional (buffer writes, commit on success, rollback on failure)
# ---------------------------------------------------------------------------


@dataclass
class PendingTransfer:
    src: str
    dst: str
    amount: int


def run_transactional(db: Database, employees: list[tuple[str, int]]) -> str:
    print("=== Transactional ===")

    pending: list[PendingTransfer] = []

    h = create_handler(get_balance, transfer, log)

    @h.on(get_balance)
    def _get(k: Resume[int, str], name: str):
        # Compute balance including uncommitted transfers
        balance = db.accounts[name].balance
        for p in pending:
            if p.src == name:
                balance -= p.amount
            if p.dst == name:
                balance += p.amount
        return k(balance)

    @h.on(transfer)
    def _transfer(k: Resume[None, str], src: str, dst: str, amount: int):
        # Buffer the operation instead of writing to DB
        pending.append(PendingTransfer(src, dst, amount))
        return k(None)

    @h.on(log)
    def _log(k: Resume[None, str], msg: str):
        print(f"  [LOG] {msg}")
        return k(None)

    result = h(lambda: process_payroll("Acme Corp", employees))

    if result == "ok":
        # Commit: apply all buffered operations at once
        print(f"  [TXN] committing {len(pending)} operations")
        for p in pending:
            db.accounts[p.src].balance -= p.amount
            db.accounts[p.dst].balance += p.amount
    else:
        # Rollback: discard all buffered operations
        print(f"  [TXN] rolling back ({len(pending)} operations discarded)")

    print(f"  result: {result}")
    print(f"  db: {db.snapshot()}")
    return result


# ---------------------------------------------------------------------------
# Handler 3: Transactional with validation (abort on overdraft)
# ---------------------------------------------------------------------------


def run_transactional_with_validation(db: Database, employees: list[tuple[str, int]]) -> str:
    print("=== Transactional + validation ===")

    pending: list[PendingTransfer] = []

    h = create_handler(get_balance, transfer, log)

    @h.on(get_balance)
    def _get(k: Resume[int, str], name: str):
        balance = db.accounts[name].balance
        for p in pending:
            if p.src == name:
                balance -= p.amount
            if p.dst == name:
                balance += p.amount
        return k(balance)

    @h.on(transfer)
    def _transfer(k: Resume[None, str], src: str, dst: str, amount: int):
        # Check balance including pending transfers
        src_balance = db.accounts[src].balance
        for p in pending:
            if p.src == src:
                src_balance -= p.amount
            if p.dst == src:
                src_balance += p.amount

        if src_balance < amount:
            # abort: return without resume -> cancels the entire transaction
            print(f"  [TXN] ABORT: {src} overdraft ({src_balance} < {amount})")
            return f"error: overdraft on {src}"

        pending.append(PendingTransfer(src, dst, amount))
        return k(None)

    @h.on(log)
    def _log(k: Resume[None, str], msg: str):
        print(f"  [LOG] {msg}")
        return k(None)

    result = h(lambda: process_payroll("Acme Corp", employees), check=False)

    if result == "ok":
        print(f"  [TXN] committing {len(pending)} operations")
        for p in pending:
            db.accounts[p.src].balance -= p.amount
            db.accounts[p.dst].balance += p.amount
    else:
        print(f"  [TXN] rolling back ({len(pending)} operations discarded)")

    print(f"  result: {result}")
    print(f"  db: {db.snapshot()}")
    return result


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main():
    employees_normal = [("Alice", 2000), ("Bob", 3000), ("Charlie", 1500)]
    employees_overdraft = [("Alice", 5000), ("Bob", 3000), ("Charlie", 4000)]  # total=12000 > 10000

    # --- Scenario 1: Normal payroll, auto-commit ---
    print("--- Scenario 1: Normal payroll ---")
    db = create_test_db()
    result = run_autocommit(db, employees_normal)
    assert result == "ok"
    assert db.accounts["Acme Corp"].balance == 3500
    print()

    # --- Scenario 2: Insufficient funds detected by business logic ---
    print("--- Scenario 2: Insufficient funds (business logic) ---")
    db = create_test_db()
    result = run_transactional(db, employees_overdraft)
    assert result == "error: insufficient funds"
    # Transactional: DB is unchanged
    assert db.accounts["Acme Corp"].balance == 10000
    print()

    # --- Scenario 3: Normal payroll, transactional ---
    print("--- Scenario 3: Normal payroll, transactional ---")
    db = create_test_db()
    result = run_transactional(db, employees_normal)
    assert result == "ok"
    assert db.accounts["Acme Corp"].balance == 3500
    print()

    # --- Scenario 4: Overdraft detected mid-transaction by handler (abort) ---
    print("--- Scenario 4: Per-transfer validation with abort ---")
    db = create_test_db()
    # Per-transfer overdraft detection: Alice(5000) passes, but Bob(3000) fails (balance 2000 < 3000)
    result = run_transactional_with_validation(db, employees_overdraft)
    assert result.startswith("error:")
    # Aborted, so DB is unchanged
    assert db.accounts["Acme Corp"].balance == 10000
    assert db.accounts["Alice"].balance == 500  # unchanged
    print()

    print("All transaction demos passed.")


if __name__ == "__main__":
    main()
