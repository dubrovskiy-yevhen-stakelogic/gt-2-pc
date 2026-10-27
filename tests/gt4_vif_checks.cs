using System;
using System.Numerics;
using PDTools.Files.Models.PS2;
static class VifChecks {
    static VIFCommand Cmd(int opcode,int address,params object[] data) {
        var c=new VIFCommand {CommandOpcode=(VIFCommandOpcode)opcode,VUAddr=(ushort)address,Num=(byte)data.Length};
        c.UnpackData.AddRange(data);return c;
    }
    static void Check(bool value) { if(!value) throw new Exception("GT4 VIF regression"); }
    static void Main() {
        // GT4 splits one position bank into multiple signed 16/8-bit delta
        // commands. Subsequent UV/face indices still address the whole bank.
        var p=new VIFPacket();
        p.Commands.Add(Cmd(0x30,0,new int[]{4096,8192,-4096,0}));
        p.Commands.Add(Cmd(5,2));
        p.Commands.Add(Cmd(0x69,0x8000,new short[]{4096,0,0},new short[]{0,-4096,4096}));
        p.Commands.Add(Cmd(0x6a,0x8002,new byte[]{255,128,127},new byte[]{1,127,129}));
        p.Commands.Add(Cmd(0x69,0x8004,new short[]{-4096,0,0}));
        p.Commands.Add(Cmd(0x65,0x8040,new short[]{0,4096})); // separate UV bank
        var v=GT2VifPositions.Decode(p);
        Check(v.Count==5 && v[0]==new Vector3(2,2,-1) && v[1]==new Vector3(2,1,0));
        Check(v[2]==new Vector3(8191/4096f,3968/4096f,127/4096f));
        Check(v[4]==new Vector3(1,4095/4096f,0));
        var offset=new VIFPacket();
        offset.Commands.Add(Cmd(0x30,0,new int[]{4096,0,0,0}));offset.Commands.Add(Cmd(5,1));
        offset.Commands.Add(Cmd(0x69,0x8000,new short[]{4096,0,0},new short[]{4096,0,0}));
        var o=GT2VifPositions.Decode(offset);Check(o[0].X==2 && o[1].X==2);
        var regular=new VIFPacket();regular.Commands.Add(Cmd(0x68,0xc000,new int[]{BitConverter.SingleToInt32Bits(-2.5f),0,0}));
        Check(GT2VifPositions.Decode(regular)[0].X==-2.5f);
        var gap=new VIFPacket();gap.Commands.Add(Cmd(0x69,0x8001,new short[]{0,0,0}));
        bool rejected=false;try { GT2VifPositions.Decode(gap); } catch(InvalidOperationException) { rejected=true; }
        Check(rejected);
        var tween=new VIFPacket();
        tween.Commands.Add(new VIFCommand{CommandOpcode=(VIFCommandOpcode)0x68,VUAddr=0xc0c0,Num=1});
        tween.Commands.Add(Cmd(0x68,0xc000,new int[]{0,0,0},new int[]{0,0,0},new int[]{0,0,0}));
        tween.Commands.Add(Cmd(0x68,0xc020,new int[]{1065353216,0,0},new int[]{1065353216,0,0},new int[]{1065353216,0,0}));
        Check(GT2VifPositions.Decode(tween).Count==3 && GT2VifPositions.Decode(tween)[0]==Vector3.Zero);
        var fixed32=new VIFPacket();fixed32.Commands.Add(Cmd(0x64,0x8040,new int[]{4096,-41755}));
        Check(GT2VifPositions.DecodeUvs(fixed32)[0]==new Vector2(1,1+41755/4096f));
        var compressedNormal=new VIFPacket();compressedNormal.Commands.Add(Cmd(0x6a,0x8080,new byte[]{127,0,129}));
        Check(GT2VifPositions.DecodeNormals(compressedNormal)[0]==new Vector3(1,0,-1));
        var uv=new VIFPacket();
        uv.Commands.Add(Cmd(0x30,0,new int[]{1024,2048,0,0}));uv.Commands.Add(Cmd(5,2));
        uv.Commands.Add(Cmd(0x65,0x8040,new short[]{1024,-1024}));
        uv.Commands.Add(Cmd(0x66,0x8041,new byte[]{128,127}));
        var t=GT2VifPositions.DecodeUvs(uv);
        Check(t.Count==2 && t[0]==new Vector2(.5f,.75f) && t[1]==new Vector2(1920/4096f,1-1151/4096f));
        // An untextured packet must not shift the UV indices of the next one.
        var shape=new PGLUshape();
        for(int i=0;i<2;++i) {
            var packet=new VIFPacket();
            packet.Commands.Add(Cmd(0x69,0x8000,new short[]{0,0,0},new short[]{4096,0,0},new short[]{0,4096,0}));
            packet.Commands.Add(Cmd(0x6e,0xc040,new byte[]{0,0,0,0},new byte[]{3,0,0,0}));
            if(i==1) packet.Commands.Add(Cmd(0x65,0x8040,new short[]{0,0},new short[]{4096,0},new short[]{0,4096}));
            shape.VIFPackets.Add(packet);shape.VIFDescriptors.Add(new VIFDescriptor{pgluTextureIndex=(ushort)i});
        }
        var mesh=shape.GetShapeData();
        Check(mesh.Vertices.Count==6 && mesh.UVs.Count==6 && mesh.Faces.Count==2);
        Check(mesh.UVs[0]==Vector2.Zero && mesh.UVs[3]==new Vector2(0,1) && mesh.UVs[4]==new Vector2(1,1));
        Check(mesh.Faces[1].A==4 && mesh.Faces[1].C==6);
        Console.WriteLine("PASS: split position/UV banks, signed delta/ROW/float decoding, missing-vertex rejection, mixed textured/untextured face indices.");
    }
}
