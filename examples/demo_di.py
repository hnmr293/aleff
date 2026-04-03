"""
Demo: User Registration Service (Dependency Injection)

Separate business logic from side effects (DB, email, logging) using algebraic effects.
Run the same business logic with different handler implementations (production, testing, abort).
"""

from dataclasses import dataclass

from aleff1 import (
    effect,
    Effect,
    Resume,
    ResumeAsync,
    create_handler,
    create_async_handler,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


@dataclass
class User:
    name: str
    email: str


@dataclass
class UserRecord:
    id: int
    name: str
    email: str


# ---------------------------------------------------------------------------
# Effects
# ---------------------------------------------------------------------------

find_user: Effect[[str], UserRecord | None] = effect("find_user")
save_user: Effect[[User], UserRecord] = effect("save_user")
send_email: Effect[[str, str, str], None] = effect("send_email")  # to, subject, body
log: Effect[[str], None] = effect("log")


# ---------------------------------------------------------------------------
# Business logic (pure — no side effects)
# ---------------------------------------------------------------------------


def register_user(name: str, email: str) -> UserRecord:
    log(f"registering user: {name} <{email}>")

    existing = find_user(email)
    if existing is not None:
        log(f"user already exists: {existing.id}")
        return existing

    user = User(name=name, email=email)
    record = save_user(user)

    log(f"user created: {record.id}")

    send_email(
        email,
        "Welcome!",
        f"Hello {name}, your account has been created (id={record.id}).",
    )

    log(f"welcome email sent to {email}")

    return record


# ---------------------------------------------------------------------------
# Handler 1: In-memory (for testing)
# ---------------------------------------------------------------------------


def run_with_inmemory_handler():
    print("=== In-memory handler ===")

    db: dict[str, UserRecord] = {}
    emails_sent: list[tuple[str, str, str]] = []
    next_id = 1

    h = create_handler(find_user, save_user, send_email, log)

    @h.on(log)
    def _log(k: Resume[None, UserRecord], msg: str):
        print(f"  [LOG] {msg}")
        return k(None)

    @h.on(find_user)
    def _find(k: Resume[UserRecord | None, UserRecord], email: str):
        return k(db.get(email))

    @h.on(save_user)
    def _save(k: Resume[UserRecord, UserRecord], user: User):
        nonlocal next_id
        record = UserRecord(id=next_id, name=user.name, email=user.email)
        next_id += 1
        db[user.email] = record
        return k(record)

    @h.on(send_email)
    def _send(k: Resume[None, UserRecord], to: str, subject: str, body: str):
        emails_sent.append((to, subject, body))
        return k(None)

    # First call: new registration
    record = h(lambda: register_user("Alice", "alice@example.com"))
    print(f"  result: {record}")

    # Second call: duplicate email -> returns existing user
    record2 = h(lambda: register_user("Alice2", "alice@example.com"))
    print(f"  result: {record2}")
    assert record2.id == record.id

    print(f"  db: {db}")
    print(f"  emails sent: {len(emails_sent)}")
    assert len(emails_sent) == 1  # only the first call
    print()


# ---------------------------------------------------------------------------
# Handler 2: Async (simulated external services)
# ---------------------------------------------------------------------------


async def run_with_async_handler():
    import asyncio

    print("=== Async handler ===")

    h = create_async_handler(find_user, save_user, send_email, log)

    @h.on(log)
    async def _log(k: ResumeAsync[None, UserRecord], msg: str):
        print(f"  [LOG] {msg}")
        return await k(None)

    @h.on(find_user)
    async def _find(k: ResumeAsync[UserRecord | None, UserRecord], email: str):
        await asyncio.sleep(0.01)  # simulate DB query
        print(f"  [DB] SELECT * FROM users WHERE email = '{email}'")
        return await k(None)  # always "not found" for demo

    @h.on(save_user)
    async def _save(k: ResumeAsync[UserRecord, UserRecord], user: User):
        await asyncio.sleep(0.01)  # simulate DB insert
        record = UserRecord(id=42, name=user.name, email=user.email)
        print(f"  [DB] INSERT INTO users ... -> id={record.id}")
        return await k(record)

    @h.on(send_email)
    async def _send(k: ResumeAsync[None, UserRecord], to: str, subject: str, body: str):
        await asyncio.sleep(0.01)  # simulate SMTP
        print(f"  [SMTP] To: {to}, Subject: {subject}")
        return await k(None)

    record = await h(lambda: register_user("Bob", "bob@example.com"))
    print(f"  result: {record}")
    print()


# ---------------------------------------------------------------------------
# Handler 3: Abort (validation that short-circuits)
# ---------------------------------------------------------------------------


def run_with_abort_handler():
    print("=== Abort handler (validation) ===")

    h = create_handler(find_user, save_user, send_email, log)

    @h.on(log)
    def _log(k: Resume[None, str], msg: str):
        return k(None)

    @h.on(find_user)
    def _find(k: Resume[UserRecord | None, str], email: str):
        # Simulate: user already exists → abort with error message
        return "error: user already exists"  # no resume → abort

    @h.on(save_user)
    def _save(k: Resume[UserRecord, str], user: User):
        return k(UserRecord(id=1, name=user.name, email=user.email))

    @h.on(send_email)
    def _send(k: Resume[None, str], to: str, subject: str, body: str):
        return k(None)

    result = h(lambda: register_user("Charlie", "charlie@example.com"), check=False)
    print(f"  result: {result}")
    assert result == "error: user already exists"
    print()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


if __name__ == "__main__":
    import asyncio

    run_with_inmemory_handler()
    asyncio.run(run_with_async_handler())
    run_with_abort_handler()

    print("All demos passed.")
