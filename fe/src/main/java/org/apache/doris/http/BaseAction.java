// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.http;

import org.apache.doris.catalog.Catalog;
import org.apache.doris.cluster.ClusterNamespace;
import org.apache.doris.common.DdlException;
import org.apache.doris.mysql.privilege.PrivPredicate;
import org.apache.doris.qe.QeService;
import org.apache.doris.system.SystemInfoService;

import com.google.common.base.Preconditions;
import com.google.common.base.Strings;

import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.io.UnsupportedEncodingException;
import java.nio.ByteBuffer;
import java.util.List;
import java.util.Map;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.ChannelFuture;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelProgressiveFuture;
import io.netty.channel.ChannelProgressiveFutureListener;
import io.netty.channel.DefaultFileRegion;
import io.netty.handler.codec.base64.Base64;
import io.netty.handler.codec.http.Cookie;
import io.netty.handler.codec.http.DefaultFullHttpResponse;
import io.netty.handler.codec.http.DefaultHttpResponse;
import io.netty.handler.codec.http.FullHttpResponse;
import io.netty.handler.codec.http.HttpChunkedInput;
import io.netty.handler.codec.http.HttpHeaders;
import io.netty.handler.codec.http.HttpMethod;
import io.netty.handler.codec.http.HttpResponse;
import io.netty.handler.codec.http.HttpResponseStatus;
import io.netty.handler.codec.http.HttpVersion;
import io.netty.handler.codec.http.LastHttpContent;
import io.netty.handler.codec.http.ServerCookieEncoder;
import io.netty.handler.ssl.SslHandler;
import io.netty.handler.stream.ChunkedFile;
import io.netty.util.CharsetUtil;

public abstract class BaseAction implements IAction {
    private static final Logger LOG = LogManager.getLogger(BaseAction.class);

    protected QeService qeService = null;
    protected ActionController controller;
    protected Catalog catalog;

    public BaseAction(ActionController controller) {
        this.controller = controller;
        // TODO(zc): remove this instance
        this.catalog = Catalog.getInstance();
    }

    public QeService getQeService() {
        return qeService;
    }

    public void setQeService(QeService qeService) {
        this.qeService = qeService;
    }

    @Override
    public void handleRequest(BaseRequest request) throws Exception {
        BaseResponse response = new BaseResponse();
        LOG.info("receive http request. url={}", request.getRequest().uri());
        try {
            execute(request, response);
        } catch (Exception e) {
            LOG.warn("fail to process url: {}", request.getRequest().uri(), e);
            if (e instanceof UnauthorizedException) {
                response.updateHeader(HttpHeaders.Names.WWW_AUTHENTICATE, "Basic realm=\"\"");
                writeResponse(request, response, HttpResponseStatus.UNAUTHORIZED);
            } else {
                writeResponse(request, response, HttpResponseStatus.NOT_FOUND);
            }
        }
    }

    public abstract void execute(BaseRequest request, BaseResponse response) throws DdlException;

    protected void writeResponse(BaseRequest request, BaseResponse response, HttpResponseStatus status) {
        // if (HttpHeaders.is100ContinueExpected(request.getRequest())) {
        // ctx.write(new DefaultFullHttpResponse(HttpVersion.HTTP_1_1,
        // HttpResponseStatus.CONTINUE));
        // }

        FullHttpResponse responseObj = null;
        try {
            responseObj = new DefaultFullHttpResponse(HttpVersion.HTTP_1_1, status,
                    Unpooled.wrappedBuffer(response.getContent().toString().getBytes("UTF-8")));
        } catch (UnsupportedEncodingException e) {
            LOG.warn("get exception.", e);
            responseObj = new DefaultFullHttpResponse(HttpVersion.HTTP_1_1, status,
                    Unpooled.wrappedBuffer(response.getContent().toString().getBytes()));
        }
        Preconditions.checkNotNull(responseObj);
        HttpMethod method = request.getRequest().method();

        checkDefaultContentTypeHeader(response, responseObj);
        if (!method.equals(HttpMethod.HEAD)) {
            response.updateHeader(HttpHeaders.Names.CONTENT_LENGTH,
                    String.valueOf(responseObj.content().readableBytes()));
        }
        writeCustomHeaders(response, responseObj);
        writeCookies(response, responseObj);

        boolean keepAlive = HttpHeaders.isKeepAlive(request.getRequest());
        if (!keepAlive) {
            request.getContext().write(responseObj).addListener(ChannelFutureListener.CLOSE);
        } else {
            responseObj.headers().set(HttpHeaders.Names.CONNECTION, HttpHeaders.Values.KEEP_ALIVE);
            request.getContext().write(responseObj);
        }
    }

    protected void writeFileResponse(BaseRequest request, BaseResponse response, HttpResponseStatus status,
            File resFile) {
        HttpResponse responseObj = new DefaultHttpResponse(HttpVersion.HTTP_1_1, status);

        if (HttpHeaders.isKeepAlive(request.getRequest())) {
            response.updateHeader(HttpHeaders.Names.CONNECTION, HttpHeaders.Values.KEEP_ALIVE);
        }

        ChannelFuture sendFileFuture;
        ChannelFuture lastContentFuture;

        // Read and return file content
        RandomAccessFile rafFile;
        try {
            rafFile = new RandomAccessFile(resFile, "r");
            long fileLength = 0;
            fileLength = rafFile.length();
            response.updateHeader(HttpHeaders.Names.CONTENT_LENGTH, String.valueOf(fileLength));
            writeCookies(response, responseObj);
            writeCustomHeaders(response, responseObj);

            // Write headers
            request.getContext().write(responseObj);

            // Write file
            if (request.getContext().pipeline().get(SslHandler.class) == null) {
                sendFileFuture = request.getContext().write(new DefaultFileRegion(rafFile.getChannel(), 0, fileLength),
                        request.getContext().newProgressivePromise());
                // Write the end marker.
                lastContentFuture = request.getContext().writeAndFlush(LastHttpContent.EMPTY_LAST_CONTENT);
            } else {
                sendFileFuture = request.getContext().writeAndFlush(
                        new HttpChunkedInput(new ChunkedFile(rafFile, 0, fileLength, 8192)),
                        request.getContext().newProgressivePromise());
                // HttpChunkedInput will write the end marker (LastHttpContent)
                // for us.
                lastContentFuture = sendFileFuture;
            }
        } catch (FileNotFoundException ignore) {
            writeResponse(request, response, HttpResponseStatus.NOT_FOUND);
            return;
        } catch (IOException e1) {
            writeResponse(request, response, HttpResponseStatus.INTERNAL_SERVER_ERROR);
            return;
        }

        sendFileFuture.addListener(new ChannelProgressiveFutureListener() {
            @Override
            public void operationProgressed(ChannelProgressiveFuture future, long progress, long total) {
                if (total < 0) { // total unknown
                    LOG.debug("{} Transfer progress: {}", future.channel(), progress);
                } else {
                    LOG.debug("{} Transfer progress: {} / {}", future.channel(), progress, total);
                }
            }

            @Override
            public void operationComplete(ChannelProgressiveFuture future) {
                LOG.debug("{} Transfer complete.", future.channel());
                if (!future.isSuccess()) {
                    Throwable cause = future.cause();
                    LOG.error("something wrong. ", cause);
                }
            }
        });

        // Decide whether to close the connection or not.
        boolean keepAlive = HttpHeaders.isKeepAlive(request.getRequest());
        if (!keepAlive) {
            // Close the connection when the whole content is written out.
            lastContentFuture.addListener(ChannelFutureListener.CLOSE);
        }
    }

    // Set 'CONTENT_TYPE' header if it havn't been set.
    protected void checkDefaultContentTypeHeader(BaseResponse response, Object responseOj) {
        if (!Strings.isNullOrEmpty(response.getContentType())) {
            response.updateHeader(HttpHeaders.Names.CONTENT_TYPE, response.getContentType());
        } else {
            response.updateHeader(HttpHeaders.Names.CONTENT_TYPE, "text/html");
        }
    }

    protected void writeCustomHeaders(BaseResponse response, HttpResponse responseObj) {
        for (Map.Entry<String, List<String>> entry : response.getCustomHeaders().entrySet()) {
            responseObj.headers().add(entry.getKey(), entry.getValue());
        }
    }

    protected void writeCookies(BaseResponse response, HttpResponse responseObj) {
        for (Cookie cookie : response.getCookies()) {
            responseObj.headers().add(HttpHeaders.Names.SET_COOKIE, ServerCookieEncoder.encode(cookie));
        }
    }

    public static class AuthorizationInfo {
        public String fullUserName;
        public String remoteIp;
        public String password;
        public String cluster;

        @Override
        public String toString() {
            StringBuilder sb = new StringBuilder();
            sb.append("user: ").append(fullUserName).append(", remote ip: ").append(remoteIp);
            sb.append(", password: ").append(password).append(", cluster: ").append(cluster);
            return sb.toString();
        }
    }

    protected void checkGlobalAuth(AuthorizationInfo authInfo, PrivPredicate predicate) throws UnauthorizedException {
        if (!Catalog.getCurrentCatalog().getAuth().checkGlobalPriv(authInfo.remoteIp,
                                                                   authInfo.fullUserName,
                                                                   predicate)) {
            throw new UnauthorizedException("Access denied; you need (at least one of) the "
                    + predicate.getPrivs().toString() + " privilege(s) for this operation");
        }
    }

    protected void checkDbAuth(AuthorizationInfo authInfo, String db, PrivPredicate predicate)
            throws UnauthorizedException {
        if (!Catalog.getCurrentCatalog().getAuth().checkDbPriv(authInfo.remoteIp, db, authInfo.fullUserName,
                                                               predicate)) {
            throw new UnauthorizedException("Access denied; you need (at least one of) the "
                    + predicate.getPrivs().toString() + " privilege(s) for this operation");
        }
    }

    protected void checkTblAuth(AuthorizationInfo authInfo, String db, String tbl, PrivPredicate predicate) 
            throws UnauthorizedException {
        if (!Catalog.getCurrentCatalog().getAuth().checkTblPriv(authInfo.remoteIp, db, authInfo.fullUserName,
                                                                tbl, predicate)) {
            throw new UnauthorizedException("Access denied; you need (at least one of) the "
                    + predicate.getPrivs().toString() + " privilege(s) for this operation");
        }
    }

    protected void checkPassword(AuthorizationInfo authInfo)
            throws UnauthorizedException {
        if (!Catalog.getCurrentCatalog().getAuth().checkPlainPassword(authInfo.fullUserName,
                                                                      authInfo.remoteIp,
                                                                      authInfo.password)) {
            throw new UnauthorizedException("Access denied for "
                    + authInfo.fullUserName + "@" + authInfo.remoteIp);
        }
    }

    public AuthorizationInfo getAuthorizationInfo(BaseRequest request)
            throws UnauthorizedException {
        AuthorizationInfo authInfo = new AuthorizationInfo();
        if (!parseAuthInfo(request, authInfo)) {
            throw new UnauthorizedException("Need auth information.");
        }
        LOG.debug("get auth info: {}", authInfo);
        return authInfo;
    }

    private boolean parseAuthInfo(BaseRequest request, AuthorizationInfo authInfo) {
        String encodedAuthString = request.getAuthorizationHeader();
        if (Strings.isNullOrEmpty(encodedAuthString)) {
            return false;
        }
        String[] parts = encodedAuthString.split(" ");
        if (parts.length != 2) {
            return false;
        }
        encodedAuthString = parts[1];
        ByteBuf buf = null;
        ByteBuf decodeBuf = null;
        try {
            buf = Unpooled.copiedBuffer(ByteBuffer.wrap(encodedAuthString.getBytes()));

            // The authString is a string connecting user-name and password with
            // a colon(':')
            decodeBuf = Base64.decode(buf);
            String authString = decodeBuf.toString(CharsetUtil.UTF_8);
            // Note that password may contain colon, so can not simply use a
            // colon to split.
            int index = authString.indexOf(":");
            authInfo.fullUserName = authString.substring(0, index);
            final String[] elements = authInfo.fullUserName.split("@");
            if (elements != null && elements.length < 2) {
                authInfo.fullUserName = ClusterNamespace.getFullName(SystemInfoService.DEFAULT_CLUSTER,
                                                                     authInfo.fullUserName);
                authInfo.cluster = SystemInfoService.DEFAULT_CLUSTER;
            } else if (elements != null && elements.length == 2) {
                authInfo.fullUserName = ClusterNamespace.getFullName(elements[1], elements[0]);
                authInfo.cluster = elements[1];
            }
            authInfo.password = authString.substring(index + 1);
            authInfo.remoteIp = request.getHostString();
        } finally {
            // release the buf and decode buf after using Unpooled.copiedBuffer
            // or it will get memory leak
            if (buf != null) {
                buf.release();
            }

            if (decodeBuf != null) {
                decodeBuf.release();
            }
        }
        return true;
    }

    protected int checkIntParam(String strParam) {
        return Integer.parseInt(strParam);
    }

    protected long checkLongParam(String strParam) {
        return Long.parseLong(strParam);
    }
}