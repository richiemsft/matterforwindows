/*
 * Copyright (c) 2026 Project CHIP Authors
 * SPDX-License-Identifier: Apache-2.0
 */
"use strict";

const video  = document.getElementById("video");
const status = document.getElementById("status");
let receiver;
let sender;
let began    = false;
let answered = false;
let checking = false;
let timer;
let drawing;
let capture;
let seenFrame           = false;
let stopped             = false;
let providerMode        = false;
const pendingCandidates = [];

function emit(message)
{
  window.chrome.webview.postMessage(JSON.stringify(message));
}

function fail(error)
{
  if (stopped) {
    return;
  }
  status.textContent = "Media failed. See native controller diagnostics.";
  emit({ type : "error", message : String(error.message || error).slice(0, 1024) });
  stop();
}

function stop()
{
  if (stopped) {
    return;
  }
  stopped = true;
  clearInterval(timer);
  clearInterval(drawing);
  if (capture) {
    for (const track of capture.getTracks()) {
      track.stop();
    }
  }
  if (receiver) {
    receiver.close();
  }
  if (sender) {
    sender.close();
  }
  video.srcObject = null;
}

function selectH264(transceiver, direction)
{
  const capabilities = direction === "send" ? RTCRtpSender.getCapabilities("video") : RTCRtpReceiver.getCapabilities("video");
  const codecs       = capabilities.codecs.filter(codec => codec.mimeType.toLowerCase() === "video/h264");
  if (!codecs.length) {
    throw new Error("H.264 unavailable in this WebView2 runtime");
  }
  transceiver.setCodecPreferences(codecs);
}

async function inspectFrames()
{
  if (checking || !receiver || receiver.connectionState === "closed") {
    return;
  }
  checking = true;
  try {
    const stats = await receiver.getStats();
    for (const report of stats.values()) {
      if (sender && report.type === "inbound-rtp") {
        emit({
          type : "self-test-stats",
          kind : report.kind,
          frames : report.framesDecoded,
          seen : seenFrame,
          width : video.videoWidth,
          height : video.videoHeight
        });
      }
      if (report.type === "inbound-rtp" && report.kind === "video" && report.framesDecoded > 0 && seenFrame) {
        const codec = stats.get(report.codecId);
        if (!codec || codec.mimeType.toLowerCase() !== "video/h264") {
          throw new Error("Decoded codec does not match the allocated H.264 stream");
        }
        if (!(video.videoWidth > 0 && video.videoHeight > 0)) {
          continue;
        }
        emit({
          type : "decoded",
          codec : "video/H264",
          frames : report.framesDecoded,
          width : video.videoWidth,
          height : video.videoHeight
        });
        status.textContent = `H.264 decoded: ${video.videoWidth} x ${video.videoHeight}`;
      }
    }
  } finally {
    checking = false;
  }
}

async function addCandidate(candidate)
{
  if (!candidate || typeof candidate.candidate !== "string" || candidate.candidate.length > 2048
      || (candidate.sdpMid !== null && typeof candidate.sdpMid !== "string")
      || (candidate.sdpMid !== null && candidate.sdpMid.length > 256)
      || (candidate.sdpMLineIndex !== null
          && (!Number.isInteger(candidate.sdpMLineIndex) || candidate.sdpMLineIndex < 0 || candidate.sdpMLineIndex > 65535))) {
    throw new Error("Invalid remote ICE candidate");
  }
  if (providerMode) {
    if (!sender || !sender.remoteDescription) {
      throw new Error("Test provider has no offer");
    }
    await sender.addIceCandidate(candidate);
  } else if (!answered) {
    if (pendingCandidates.length >= 128) {
      throw new Error("Remote ICE queue exhausted");
    }
    pendingCandidates.push(candidate);
  } else {
    await receiver.addIceCandidate(candidate);
  }
}

async function answer(sdp)
{
  if (!receiver || answered || typeof sdp !== "string" || !sdp.length || sdp.length > 16384) {
    throw new Error("Invalid or duplicate SDP answer");
  }
  await receiver.setRemoteDescription({ type : "answer", sdp });
  answered = true;
  for (const candidate of pendingCandidates.splice(0)) {
    await receiver.addIceCandidate(candidate);
  }
}

async function createTestSender(offer, candidateCallback)
{
  if (sender) {
    throw new Error("Duplicate test-provider offer");
  }
  sender                = new RTCPeerConnection({ iceServers : [] });
  sender.onicecandidate = event => {
    if (event.candidate) {
      candidateCallback(event.candidate.toJSON());
    }
  };
  await sender.setRemoteDescription(offer);
  const canvas          = document.createElement("canvas");
  canvas.width          = 640;
  canvas.height         = 480;
  const context         = canvas.getContext("2d");
  let frame             = 0;
  drawing               = setInterval(() => {
    context.fillStyle = frame++ % 2 ? "#00aaff" : "#ff5500";
    context.fillRect(0, 0, canvas.width, canvas.height);
                }, 1000 / 30);
  capture               = canvas.captureStream(30);
  const transceiver     = sender.getTransceivers()[0];
  transceiver.direction = "sendonly";
  await transceiver.sender.replaceTrack(capture.getVideoTracks()[0]);
  selectH264(transceiver, "send");
  await sender.setLocalDescription(await sender.createAnswer());
  if (sender.localDescription.sdp.length > 16384) {
    throw new Error("Test-provider SDP exceeds Matter signaling limit");
  }
  return sender.localDescription.sdp;
}

async function selfTest(offer)
{
  const senderCandidates  = [];
  receiver.onicecandidate = event => {
    if (event.candidate) {
      if (sender.remoteDescription) {
        sender.addIceCandidate(event.candidate).catch(fail);
      } else {
        senderCandidates.push(event.candidate);
      }
    }
  };
  const sdp = await createTestSender(offer, candidate => addCandidate(candidate).catch(fail));
  for (const candidate of senderCandidates.splice(0)) {
    await sender.addIceCandidate(candidate);
  }
  await answer(sdp);
}

async function begin(test, testProvider)
{
  if (began) {
    throw new Error("Duplicate media start");
  }
  began        = true;
  providerMode = testProvider;
  if (providerMode) {
    emit({ type : "provider-ready" });
    return;
  }
  receiver          = new RTCPeerConnection({ iceServers : [], bundlePolicy : "max-bundle" });
  const transceiver = receiver.addTransceiver("video", { direction : "recvonly" });
  selectH264(transceiver, "receive");
  receiver.ontrack = event => {
    if (event.track.kind !== "video") {
      fail(new Error("Unexpected non-video track"));
      return;
    }
    video.srcObject = new MediaStream([ event.track ]);
    video.requestVideoFrameCallback(() => { seenFrame = true; });
    video.play().catch(fail);
  };
  receiver.onconnectionstatechange = () => {
    if (receiver.connectionState === "failed") {
      fail(new Error("WebRTC connection failed"));
    }
    if (receiver.connectionState === "disconnected") {
      emit({ type : "disconnected" });
    }
    if (receiver.connectionState === "connected") {
      emit({ type : "connected" });
    }
  };
  const localCandidates = [];
  let offerPosted       = false;
  const sendCandidate = candidate => emit({ type : "candidate", candidate });
  receiver.onicecandidate = event => {
    if (!event.candidate) {
      return;
    }
    if (offerPosted) {
      sendCandidate(event.candidate.toJSON());
    } else {
      localCandidates.push(event.candidate.toJSON());
    }
  };
  timer = setInterval(() => inspectFrames().catch(fail), 500);
  await receiver.setLocalDescription(await receiver.createOffer());
  if (receiver.localDescription.sdp.length > 16384) {
    throw new Error("SDP exceeds Matter signaling limit");
  }
  if (test) {
    emit({ type : "self-test-started" });
    await selfTest(receiver.localDescription);
  } else {
    emit({ type : "offer", sdp : receiver.localDescription.sdp });
    offerPosted = true;
    for (const candidate of localCandidates) {
      sendCandidate(candidate);
    }
  }
}

let sequence = Promise.resolve();
window.chrome.webview.addEventListener("message", event => {
  sequence = sequence
                 .then(async () => {
                   const message = event.data;
                   switch (message.type) {
                   case "begin":
                     await begin(message.selfTest === true, message.testProvider === true);
                     break;
                   case "offer":
                     if (!providerMode || typeof message.sdp !== "string" || !message.sdp.length || message.sdp.length > 16384) {
                       throw new Error("Unexpected test-provider offer");
                     }
                     emit({
                       type : "answer",
                       sdp : await createTestSender(
                           { type : "offer", sdp : message.sdp }, candidate => emit({ type : "candidate", candidate }))
                     });
                     break;
                   case "answer":
                     await answer(message.sdp);
                     break;
                   case "candidate":
                     await addCandidate(message.candidate);
                     break;
                   default:
                     throw new Error("Unknown native media message");
                   }
                 })
                 .catch(fail);
});
window.addEventListener("beforeunload", stop);
emit({ type : "ready" });
