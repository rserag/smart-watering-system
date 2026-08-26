import hashlib
import secrets
from datetime import datetime, timedelta, timezone
from typing import Annotated

from authlib.integrations.starlette_client import OAuth, OAuthError
from fastapi import APIRouter, Cookie, Depends, HTTPException, Request, Response
from fastapi.responses import RedirectResponse
from sqlalchemy.ext.asyncio import AsyncSession

from app.config import get_settings
from app.database import SessionLocal, get_session
from app.models import UserSession


settings = get_settings()
router = APIRouter(tags=["authentication"])
oauth = OAuth()
if settings.google_client_id and settings.google_client_secret:
    oauth.register(
        name="google",
        client_id=settings.google_client_id,
        client_secret=settings.google_client_secret,
        server_metadata_url="https://accounts.google.com/.well-known/openid-configuration",
        client_kwargs={"scope": "openid email profile"},
    )


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


async def create_user_session(sub: str, email: str, name: str) -> str:
    raw_token = secrets.token_urlsafe(48)
    now = utcnow()
    async with SessionLocal() as session:
        session.add(
            UserSession(
                token_hash=token_hash(raw_token),
                google_sub=sub,
                email=email.lower(),
                name=name,
                created_at=now,
                last_seen_at=now,
                expires_at=now + timedelta(hours=settings.session_ttl_hours),
            )
        )
        await session.commit()
    return raw_token


async def lookup_user_session(session: AsyncSession, raw_token: str | None) -> UserSession | None:
    if not raw_token:
        return None
    user_session = await session.get(UserSession, token_hash(raw_token))
    if user_session is None:
        return None
    if user_session.expires_at <= utcnow():
        await session.delete(user_session)
        await session.commit()
        return None
    user_session.last_seen_at = utcnow()
    await session.commit()
    return user_session


async def optional_user(
    session: Annotated[AsyncSession, Depends(get_session)],
    watering_session: Annotated[str | None, Cookie()] = None,
) -> UserSession | None:
    return await lookup_user_session(session, watering_session)


async def require_user(user: Annotated[UserSession | None, Depends(optional_user)]) -> UserSession:
    if user is None:
        raise HTTPException(status_code=401, detail="Authentication required")
    return user


def set_session_cookie(response: Response, raw_token: str) -> None:
    response.set_cookie(
        settings.session_cookie_name,
        raw_token,
        max_age=settings.session_ttl_hours * 3600,
        httponly=True,
        secure=True,
        samesite="lax",
        path="/",
    )


@router.get("/auth/google/login")
async def google_login(request: Request) -> Response:
    if settings.auth_mode == "development":
        raw_token = await create_user_session(
            "development-user",
            settings.development_user_email,
            settings.development_user_name,
        )
        response = RedirectResponse(settings.frontend_url, status_code=302)
        set_session_cookie(response, raw_token)
        return response

    if not settings.google_client_id or not settings.google_client_secret:
        raise HTTPException(status_code=503, detail="Google OAuth is not configured")
    if not settings.allowed_email_set:
        raise HTTPException(status_code=503, detail="No Google accounts are allowlisted")
    redirect_uri = str(request.url_for("google_callback"))
    return await oauth.google.authorize_redirect(request, redirect_uri)


@router.get("/auth/google/callback", name="google_callback")
async def google_callback(request: Request) -> Response:
    if settings.auth_mode != "google" or not hasattr(oauth, "google"):
        raise HTTPException(status_code=404, detail="Google OAuth is not enabled")
    try:
        token = await oauth.google.authorize_access_token(request)
    except OAuthError as exc:
        raise HTTPException(status_code=401, detail="Google sign-in failed") from exc
    user_info = token.get("userinfo")
    if not user_info:
        raise HTTPException(status_code=401, detail="Google did not return an identity")
    email = str(user_info.get("email", "")).lower()
    if not user_info.get("email_verified") or email not in settings.allowed_email_set:
        raise HTTPException(status_code=403, detail="This Google account is not allowed")
    sub = str(user_info.get("sub", ""))
    if not sub:
        raise HTTPException(status_code=401, detail="Google identity is missing a subject")
    raw_token = await create_user_session(sub, email, str(user_info.get("name") or email))
    response = RedirectResponse(settings.frontend_url, status_code=302)
    set_session_cookie(response, raw_token)
    return response


@router.post("/auth/logout", status_code=204)
async def logout(
    response: Response,
    session: Annotated[AsyncSession, Depends(get_session)],
    watering_session: Annotated[str | None, Cookie()] = None,
) -> Response:
    if watering_session:
        user_session = await session.get(UserSession, token_hash(watering_session))
        if user_session is not None:
            await session.delete(user_session)
            await session.commit()
    response.delete_cookie(settings.session_cookie_name, path="/", secure=True, httponly=True, samesite="lax")
    return response


@router.get("/api/me")
async def me(user: Annotated[UserSession | None, Depends(optional_user)]) -> dict:
    if user is None:
        return {"authenticated": False, "authMode": settings.auth_mode}
    return {
        "authenticated": True,
        "authMode": settings.auth_mode,
        "email": user.email,
        "name": user.name,
    }

