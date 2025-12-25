# BERT and BERT-RPC 1.0 Specification

**Author:** Tom Preston-Werner  
**Source:** [Feuerlabs/bert](https://github.com/Feuerlabs/bert/blob/master/priv/BERT%20and%20BERT-RPC%201.0%20Specification.html)  
**Original Site:** [bert-rpc.org](http://bert-rpc.org) (no longer available)

## Purpose

BERT and BERT-RPC are an attempt to specify a flexible binary serialization and RPC protocol that are compatible with the philosophies of dynamic languages such as Ruby, Python, PERL, JavaScript, Erlang, Lua, etc. BERT aims to be as simple as possible while maintaining support for the advanced data types we have come to know and love. BERT-RPC is designed to work seamlessly within a dynamic/agile development workflow. The BERT-RPC philosophy is to eliminate extraneous type checking, IDL specification, and code generation. This frees the developer to actually get things done.

This document represents the 1.0 specification for both BERT and BERT-RPC. The primary author of this document is [Tom Preston-Werner](http://tom.preston-werner.com), cofounder of [GitHub](http://github.com). These technologies are currently in production use within the infrastructure of GitHub and play a part in serving nearly every page of the site.

All discussion should be made via the [Mailing List](http://groups.google.com/group/bert-rpc).

## BERT

BERT (Binary ERlang Term) is a flexible binary data interchange format based on (and compatible with) Erlang's binary serialization format (as used by [erlang:term_to_binary/1](http://erldocs.com/otp_src_R13B/erts/erlang.html?search=term_to_binary&i=0#term_to_binary/1)). BERT supports the following simple data types (shown here using Erlang syntax):

| Type         | Example                                 |
| ------------ | --------------------------------------- |
| **integer**  | `4`                                     |
| **float**    | `8.1516`                                |
| **atom**     | `foo`                                   |
| **tuple**    | `{coord, 23, 42}`                       |
| **bytelist** | `[1, 2, 3]`                             |
| **list**     | `[a, [1, 2]]`                           |
| **binary**   | `<<"Roses are red\0Violets are blue">>` |

Any of the above data types may be referred to as a "term." Tuples and lists may contain heterogenous terms as their elements. For example, the tuple `{coord, 23, 42}` contains one atom and two integers, and the list `[a, [1, 2]]` contains one atom and one list.

In addition to these simple types, BERT defines standard formats for a variety of complex data types. Complex types are built with the simple primitives and are dependent upon specific BERT implementations for conversion to native data types. Not all languages will have appropriate mappings for all complex types.

All complex types are expressed as tuples where the first element is the atom `bert`. This allows for simple decoding logic and for the safe addition of future complex types while preventing ambiguous tuple definitions. As such, `bert` should be considered a reserved word when used as the first element of a tuple. Libraries should take steps to prevent users from inadvertently using `bert` in that position.

### Complex Types

| Type    | Format        |
| ------- | ------------- |
| **nil** | `{bert, nil}` |

Erlang equates `nil` with the empty array `[]` while other languages do not. Even though `NIL` appears as a primitive in the serialization specification, BERT only uses it to represent the empty array. In order to be language agnostic, `nil` is encoded as a separate complex type to allow for disambiguation.

| Type        | Format                            |
| ----------- | --------------------------------- |
| **boolean** | `{bert, true}` or `{bert, false}` |

Erlang equates the `true` and `false` atoms with booleans while other languages do not have this behavior. To disambiguate these cases, booleans are expressed as their own complex type.

| Type           | Format                        |
| -------------- | ----------------------------- |
| **dictionary** | `{bert, dict, KeysAndValues}` |

Dictionaries (hash tables) are expressed via an array of 2-tuples representing the key/value pairs. The KeysAndValues array is mandatory, such that an empty dict is expressed as `{bert, dict, []}`. Keys and values may be any term. For example, `{bert, dict, [{name, <<"Tom">>}, {age, 30}]}`.

| Type     | Format                                             |
| -------- | -------------------------------------------------- |
| **time** | `{bert, time, Megaseconds, Seconds, Microseconds}` |

The given time is the number of Megaseconds + Seconds + Microseconds elapsed since 00:00 GMT, January 1, 1970 (zero hour). For example, 2009-10-11 at 14:12:01 and 446,228 microseconds would be expressed as `{bert, time, 1255, 295581, 446228}`. In english, this is 1255 megaseconds (millions of seconds) + 295,581 seconds + 446,228 microseconds (millionths of a second) since zero hour.

| Type      | Format                           |
| --------- | -------------------------------- |
| **regex** | `{bert, regex, Source, Options}` |

Regular expressions are expressed by their source binary and PCRE options. Options is a list of atoms representing the PCRE options. For example, `{bert, regex, <<"^c(a*)t$">>, [caseless]}` would represent a case insensitive regular epxression that would match "cat". See [re:compile/2](http://erldocs.com/otp_src_R13B/stdlib/re.html?search=re:&i=1#compile/2) for valid options.

### Encoding

The BERT encoding is identical to Erlang's [external term format](http://erlang.org/doc/apps/erts/erl_ext_dist.html) except that it is restricted to the following data type identifiers: 97-100, 104-111. The encoding is a simple length-prefixed serialization. As a quick example, the following term:

```erlang
[1, 2, 3]
```

would be encoded as:

```erlang
<<131,107,0,3,1,2,3>>
```

where `<<131>>` is the "magic number" specifying the protocol version, `<<107>>` is the type identifier for a bytelist, `<<0,3>>` is the length of the list, and `<<1,2,3>>` is the bytelist contents.

## BERP

Binary ERlang Packets, or BERPs, are used for transmitting BERTs over the wire. A BERP is simply a BERT prepended with a four byte length header, where the highest order bit is first in network order. For example, a 20 byte BERT, when represented as a BERP would have the binary header:

```
00000000 00000000 00000000 00010100
```

The header would be followed by 20 bytes of BERT data, making the BERP 24 bytes overall. Reading streams of BERPs involves reading the four byte header, reading that many bytes (the BERT itself), and then starting over again.

The maximum BERT size that may be transferred as a BERP is 2^32 bytes = 4 GiB.

This document uses a special notation to denote BERPs and their source in a client/server relationship. A BERP that is sent from a client to a server starts with `->` and is followed by the BERT in Erlang syntax form:

```erlang
-> {request, foo}
```

A BERP that is sent from a server to a client starts with `<-` and is followed by the BERT in Erlang syntax form:

```erlang
<- {response, bar}
```

The easy way to remember this is to pretend that your screen is the server and that the space outside your screen to the left is the internet. Client requests come from the internet (outside and to the left of your screen) and travel in towards your screen: `->`. Server responses come from the server (your screen) and travel out towards the internet: `<-`.

When discussing BERP forms, variable data may be denoted by using capitalized words (Erlang variable syntax). For example:

```erlang
-> {call, Mod, Fun, Args}
```

In the above, the form starts with the `call` atom, and is followed by arbitrary terms in the Mod, Fun, and Args position. The nature of these terms will be specified in the associated descriptions.

## BERT-RPC

BERT-RPC is a transport-layer agnostic protocol for performing remote procedure calls using BERPs as the serialization mechanism. BERT-RPC supports caching directives, asynchronous operations, and both call and response streaming.

For the following examples, consider a photo hosting service called Photox. This use case should require enough richness to show off all the features of BERT-RPC.

### Synchronous Request

Let's start with the simplest transaction: synchronous request and response. Synchronous requests take the form:

```erlang
-> {call, Module, Function, Arguments}
```

Where Module and Function are atoms, and Arguments is a list of zero or more terms. So, to get the image size for image 99, we use the request:

```erlang
-> {call, photox, img_size, [99]}
```

If successful, the response takes for the form:

```erlang
<- {reply, Result}
```

where Result is an arbitrary term. For example:

```erlang
<- {reply, {xy, 600, 800}}
```

If there is a problem with the request, an error response is returned that takes the form:

```erlang
<- {error, {Type, Code, Class, Detail, Backtrace}}
```

where Type is an atom, Code is an integer, Class and Detail are a binaries, and Backtrace is a List of Binaries. For example, if the function is not found on the module:

```erlang
<- {error, {server, 2,
            <<"BERTError">>,
            <<"function 'img_size' not found on module 'photox'">>,
            [<<"file:line:context">>]}}
```

Valid error types are `protocol`, `server`, `user`, and `proxy`. Codes 0-99 are reserved as predefined error messages. Codes 100+ are for custom use.

#### Protocol Error Codes

```
0 = Undesignated
1 = Unable to read header
2 = Unable to read data
```

#### Server Error Codes

```
0 = Undesignated
1 = No such module
2 = No such function
```

### Asynchronous Request

Some operations are best served by making asynchronous requests that do not require any response value. For that, you can use `cast`. It takes the form:

```erlang
-> {cast, Module, Function, Arguments}
```

It operates the same way as `call` except that if it is successfully received by the server, it immediately returns:

```erlang
<- {noreply}
```

and then proceeds to execute the RPC asynchronously. If there is a protocol or server level error, the response will be a standard error tuple. Since user level errors will happen after the response is sent back to the client, they are ignored.

### Informational BERPs

The advanced protocol features of BERT-RPC rely on `info` packets that carry metadata about the transactions. These packets are only ever sent as modifiers of the immediately following BERP. They take the form:

```erlang
-> {info, Command, Options}
```

Where command is an atom and Options is an array of tuples that differ depending on the Command directive. Info packets may be sent by either the client or server and do not warrant a response of their own. If there is a problem with the info packet, an `error` response will be issued after the following normal request BERP is sent.

### Asynchronous Request with Callback

To get a return value from an asynchronous request, you can specify a callback service, module, and function that will be cast to upon completion of the asynchronous request. The `info` packet takes the form:

```erlang
-> {info, callback, [{service, Service}, {mfa, Mod, Fun, Args}]}
```

Where service is the binary of the service-specific callback address, Mod and Fun are the obvious, and Args is an array of arguments that will be prepended to the arguments that are eventually sent to the callback. For example, on a TCP transport layer:

```erlang
-> {info, callback, [{service, <<"cron.photox.biz:4815">>},
                     {mfa, cron, updated_stats, [42]}]}
-> {cast, photox, update_stats, [42]}
```

which immediately returns:

```erlang
<- {noreply}
```

and when the request is completed, makes a BERT-RPC cast to `cron:updated_stats` at cron.photox.biz:4815. If the the photox:update_stats call would have normally returned `86193`, then the BERT-RPC cast will be:

```erlang
<- {cast, cron, updated_stats, [42, 86193]}
```

Note that the `42` specified in the callback `info` packet has been prepended to the arguments. The response or error from the callback cast is ignored by the server.

### Caching Directives

BERT-RPC defines a rich set of caching directives. In all cases, the key upon which cache storage is based is always of the form:

```erlang
-> {Mod, Fun, Args}
```

We call this the "MFA Term" or "MFAT".

#### Expiration Caching

If the server decides that a response can be cached for a specific amount of time (or forever), then an info packet of the following form may be sent:

```erlang
<- {info, cache, [{access, Access}, {expiration, Time}]}
```

Access is one of `public` or `private`. Public access means that a cache at any level may cache the response. Private access means that only a local cache on the client may cache the response. Time is an integer number of seconds. If Time is 0, then the cache is valid forever. For example, given a request:

```erlang
-> {call, photox, image_size, [99]}
```

The response with caching directive may be:

```erlang
<- {info, cache, [{access, public}, {expiration, 60}]}
<- {reply, {xy, 600, 800}}
```

#### Validation Caching

For any request, the server may respond with an info packet containing a validation token:

```erlang
<- {info, cache, [{access, Access}, {validation, Token}]}
```

Access is the same as in expiration caching. Token is an arbitrary binary token that is intended to be quickly generated from server side data that indicates the freshness of the data. For example, given a request:

```erlang
-> {call, photox, comments, [99]}
```

The response with caching directive may be:

```erlang
<- {info, cache, [{access, public},
                  {validation, <<"a61bbf569169fc2f8fce">>}]}
<- {reply, [{bert, dict, {author, <<"mojombo">>},
                         {body, <<"Nice photo!">>}}]}
```

Subsequent requests for the same MFAT may send an info packet with the token that was previously sent with the response:

```erlang
-> {info, cache, [{validation, <<"a61bbf569169fc2f8fce">>}]}
-> {call, photox, comment_count, [99]}
```

Upon receiving this request, the server recalculates the token and compares it to the token that was sent in the info packet. If the token is different (meaning that the server side source data has changed) then a full response is produced and sent back (along with, presumably, an updated cache directive containing the new token). If the token is the same, the server will respond with an info packet specifying that the cached data is still valid, and a `noreply`:

```erlang
<- {info, valid, []}
<- {noreply}
```

The cached result will then be used as the actual response to the original request.

### Streaming Binary Request

Since Photox is an image hosting site, some of the service calls will need to send large amounts of binary data. BERT-RPC makes it possible to stream arbitrarily large binary requests via a chunked transfer encoding that follows the normal BERT-RPC request. For example:

```erlang
-> {info, stream, []}
-> {call, photox, send_image_data, [99]}
   <n-header><n-bytes>
   ...
   <0-header>
```

The info BERP declares that the request will be a binary stream. The second BERP contains the normal request. Immediately following that will be a four byte length header, then that many bytes of binary data. This header and data pair can occur any number of times. Once a zero length header (four null bytes) is encountered, the stream is finished and the server will issue a response:

```erlang
<- {reply, 238452}
```

In this example, it just returns the number of bytes that were transferred in the binary stream.

### Streaming Binary Response

Streaming responses follow a similar pattern to streaming requests. The difference is that the info BERP and stream happen in the response phase. For example:

```erlang
-> {call, photox, get_image_data, [99]}
```

would return:

```erlang
<- {info, stream, []}
<- {reply, []}
   <n-header><n-bytes>
   ...
   <0-header>
```

The info BERP declares that the response will be a binary stream. The second BERP contains the normal reply. Immediately following that will be a four byte length header, then that many bytes of binary data. This header and data pair can occur any number of times. Once a zero length header (four null bytes) is encountered, the stream is finished and the connection may be closed.

## Implementations

You can help improve the BERT ecosystem by implementing a BERT serializer, BERT-RPC client, or BERT-RPC server in your favorite language! Let me know of your project and I'll happily add it to this list.

### BERT Serializers

- C++ - [libbert](https://github.com/ruediger/libbert)
- Clojure - [bert-clj](https://github.com/trotter/bert-clj)
- Erlang - [bert.erl](https://github.com/mojombo/bert.erl)
- Factor - [factor-bert](https://github.com/wookay/factor-bert)
- Go - [gobert](https://github.com/josh/gobert)
- Haskell - [bert](https://github.com/mariusaeriksen/bert)
- JavaScript - [BERT-JS](https://github.com/rklophaus/BERT-JS)
- Python - [python-bert](https://github.com/samuel/python-bert)
- Ruby - [BERT](https://github.com/mojombo/bert)
- Scala - [scala-bert](https://github.com/stephenjudkins/scala-bert)
- Scheme - [scheme-bert](http://bitbucket.org/yasir/scheme-bert)

### BERT-RPC Clients

- Factor Client - [factor-bert](https://github.com/wookay/factor-bert)
- Haskell Client - [bert](https://github.com/mariusaeriksen/bert)
- Node.js Client - [node-bertrpc](https://github.com/rtomayko/node-bertrpc)
- Python Client - [python-bertrpc](https://github.com/mjrusso/python-bertrpc)
- Ruby Client - [BERTRPC](https://github.com/mojombo/bertrpc)
- Ruby (EM) Client - [bertrem](https://github.com/b/bertrem)

### BERT-RPC Servers

- Factor Server - [factor-bert](https://github.com/wookay/factor-bert)
- Haskell Server - [bert](https://github.com/mariusaeriksen/bert)
- Node.js Server - [node-bertrpc](https://github.com/rtomayko/node-bertrpc)
- Python Server - [python-ernie](https://github.com/tylerneylon/python-ernie)
- Python (Eventlet) Server - [bertlet](https://github.com/luckythetourist/bertlet)
- Ruby/Erlang Hybrid Server - [Ernie](https://github.com/mojombo/ernie)
